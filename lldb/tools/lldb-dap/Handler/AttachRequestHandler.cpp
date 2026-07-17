//===-- AttachRequestHandler.cpp ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DAP.h"
#include "DAPLog.h"
#include "EventHelper.h"
#include "LLDBUtils.h"
#include "Protocol/ProtocolRequests.h"
#include "RequestHandler.h"
#include "lldb/API/SBAttachInfo.h"
#include "lldb/API/SBListener.h"
#include "lldb/lldb-defines.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

using namespace llvm;
using namespace lldb_dap::protocol;
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
static llvm::Expected<lldb::file_t> recv_fd(int sock) {
  char dummy{};
  struct iovec iov{};
  iov.iov_base = &dummy;
  iov.iov_len = 1;

  union {
    char buf[CMSG_SPACE(sizeof(int))];
    struct cmsghdr align;
  } u{};

  struct msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = u.buf;
  msg.msg_controllen = sizeof(u.buf);

#ifdef __linux__
  const int flags = MSG_CMSG_CLOEXEC;
#else
  const int flags = 0;
#endif
  if (::recvmsg(sock, &msg, flags) < 0)
    return llvm::errorCodeToError(llvm::errnoAsErrorCode());

  if (msg.msg_controllen == 0)
    return llvm::createStringError("No ancillary data received!");

  if (msg.msg_flags & MSG_CTRUNC) {
    return llvm::createStringError("Ancillary data was truncated!");
  }

  struct cmsghdr *cmsg = NULL;
  for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; CMSG_NXTHDR(&msg, cmsg)) {
    if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
      continue;

    int fd{};
    ::memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));

    // Confirm it's a socket
    int optval;
    socklen_t optlen = sizeof(optval);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &optval, &optlen) < 0) {
      return llvm::createStringErrorV("received fd {} is not a valid socket",
                                      fd);
      return -1;
    }

    // check for errors.
    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) {
      return llvm::createStringErrorV("Socket has error: {}", strerror(err));
    }
    return fd;
  }

  // TODO: fprintf(stderr, "no file descriptor in received message\n");
  return llvm::createStringError("failed to get the file descriptor");
}

static llvm::Expected<lldb::file_t> GetFDFromSocket(const String &socket_path) {
  const char *sock_path = socket_path.c_str();

  const int sock_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock_fd < 0) {
    return llvm::errorCodeToError(llvm::errnoAsErrorCode());
  }

  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  ::strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

  if (::connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    return llvm::errorCodeToError(llvm::errnoAsErrorCode());
  }

  llvm::scope_exit cleanup([&] { ::close(sock_fd); });

  return recv_fd(sock_fd);
}

#include <netinet/in.h>
std::string getFdType(int fd) {
  struct stat sb;

  if (fstat(fd, &sb) == -1) {
    return "error";
  }
  switch (sb.st_mode & S_IFMT) {
  case S_IFREG:
    return "regular file";
  case S_IFDIR:
    return "directory";
  case S_IFLNK:
    return "symbolic link";
  case S_IFCHR:
    return "character device";
  case S_IFBLK:
    return "block device";
  case S_IFIFO:
    return "FIFO/pipe";
  case S_IFSOCK:
    return "socket";
  default:
    return "unknown";
  }
}

std::string diagnoseFd(int fd) {
  std::string result;

  // Check if fd is valid
  struct stat sb;
  if (fstat(fd, &sb) == -1) {
    return "fstat failed: " + std::string(strerror(errno));
  }

  // Check fd type
  if (!S_ISSOCK(sb.st_mode)) {
    return "fd is not a socket (type: " + getFdType(fd) + ")";
  }

  // Check socket type
  int type = 0;
  socklen_t len = sizeof(type);
  if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) == -1) {
    result += "SO_TYPE failed: " + std::string(strerror(errno)) + " | ";
  } else {
    switch (type) {
    case SOCK_STREAM:
      result += "SOCK_STREAM | ";
      break;
    case SOCK_DGRAM:
      result += "SOCK_DGRAM | ";
      break;
    case SOCK_RAW:
      result += "SOCK_RAW | ";
      break;
    default:
      result += "type=" + std::to_string(type) + " | ";
    }
  }

  // Check socket domain
  struct sockaddr_storage addr;
  socklen_t addrlen = sizeof(addr);
  if (getsockname(fd, (struct sockaddr *)&addr, &addrlen) == -1) {
    result += "getsockname failed: " + std::string(strerror(errno));
  } else {
    switch (addr.ss_family) {
    case AF_INET:
      result += "AF_INET";
      break;
    case AF_INET6:
      result += "AF_INET6";
      break;
    case AF_UNIX:
      result += "AF_UNIX";
      break;
    default:
      result += "family=" + std::to_string(addr.ss_family);
    }
  }

  return result;
}

namespace lldb_dap {

/// The `attach` request is sent from the client to the debug adapter to attach
/// to a debuggee that is already running.
///
/// Since attaching is debugger/runtime specific, the arguments for this request
/// are not part of this specification.
Error AttachRequestHandler::Run(const AttachRequestArguments &args) const {
  // Initialize DAP debugger and related components if not sharing previously
  // launched debugger.
  std::optional<DAPSession> session = args.session;

  if (Error err =
          session ? dap.InitializeDebugger(*session) : dap.InitializeDebugger())
    return err;

  dap.SetConfiguration(args.configuration, /*is_attach=*/true);
  if (!args.coreFile.empty()) {
    dap.stop_at_entry = true;
    dap.is_live_session = false;
  }

  PrintWelcomeMessage();

  // This is a hack for loading DWARF in .o files on Mac where the .o files
  // in the debug map of the main executable have relative paths which
  // require the lldb-dap binary to have its working directory set to that
  // relative root for the .o files in order to be able to load debug info.
  if (!dap.configuration.debuggerRoot.empty())
    sys::fs::set_current_path(dap.configuration.debuggerRoot);

  // Run any initialize LLDB commands the user specified in the launch.json
  if (Error err = dap.RunInitCommands())
    return err;

  dap.ConfigureSourceMaps();

  lldb::SBError error;
  lldb::SBTarget target;
  if (session) {
    // Use the unique target ID to get the target.
    target = dap.debugger.FindTargetByGloballyUniqueID(session->targetId);
    if (!target.IsValid()) {
      error.SetErrorString(
          llvm::formatv("invalid targetId {0} in attach config",
                        session->targetId)
              .str()
              .c_str());
    }
  } else {
    target = dap.CreateTarget(error);
  }

  if (target.IsValid())
    dap.SetTarget(target);

  // Run any pre run LLDB commands the user specified in the launch.json
  if (Error err = dap.RunPreRunCommands())
    return err;

  if ((args.pid == LLDB_INVALID_PROCESS_ID ||
       args.gdbRemotePort == LLDB_DAP_INVALID_PORT) &&
      args.waitFor && !args.configuration.program.empty())
    dap.SendOutput(
        OutputType::Console,
        llvm::formatv("Waiting to attach to \"{0}\"...\n",
                      llvm::sys::path::filename(dap.configuration.program))
            .str());

  {
    // Perform the launch in synchronous mode so that we don't have to worry
    // about process state changes during the launch.
    ScopeSyncMode scope_sync_mode(dap.debugger);

    if (!args.attachCommands.empty()) {
      // treat the first line the socket to connect to as I don't want to to all
      // the setup
      const auto &socket_path = args.attachCommands[0];
      llvm::Expected<lldb::file_t> exp_fd = GetFDFromSocket(socket_path);
      if (llvm::Error error = exp_fd.takeError()) {
        return error;
      }

      const auto dbg_server_fd = *exp_fd;
      const auto fd_type = getFdType(dbg_server_fd);
      DAP_LOG(dap.log, "fd type : {}\n", fd_type);
      dap.SendOutput(OutputType::Console,
                     llvm::formatv("fd type : {}", fd_type).str());

      dap.SendOutput(
          OutputType::Console,
          llvm::formatv("debug server fd type : {}", dbg_server_fd).str());

      int opt = 1;
      if (::setsockopt(dbg_server_fd, SOL_SOCKET, SO_NOSIGPIPE, &opt,
                       sizeof(opt)) == -1) {
        auto result = std::string("FD: ") + diagnoseFd(dbg_server_fd);
        DAP_LOG(dap.log, "fd type : {}\n", result);
      }
      const std::string connect_url = llvm::formatv("fd://{}", dbg_server_fd);
      lldb::SBListener listener = dap.debugger.GetListener();
      auto process = dap.target.ConnectRemote(listener, connect_url.c_str(),
                                              "gdb-remote", error);

      const auto id = process.GetProcessID();
      dap.SendOutput(OutputType::Console,
                     llvm::formatv(" process id {}", id).str());

      if (args.pid != LLDB_INVALID_PROCESS_ID) {
        dap.SendOutput(
            OutputType::Console,
            llvm::formatv("\nattaching to pid process id {}\n", args.pid)
                .str());
        process.RemoteAttachToProcessWithID(args.pid, error);
        dap.SendOutput(
            OutputType::Console,
            llvm::formatv("Attaching Error: {}\n", error.GetCString()).str());
      }
      // lldb::SBAttachInfo attach_info;
      // if (args.pid != LLDB_INVALID_PROCESS_ID)
      //   attach_info.SetProcessID(args.pid);
      // else if (!dap.configuration.program.empty())
      //   attach_info.SetExecutable(dap.configuration.program.data());
      // attach_info.SetWaitForLaunch(args.waitFor, /*async=*/false);
      // dap.target.Attach(attach_info, error);
      // dap.SendOutput(OutputType::Console,
      //                llvm::formatv("Attach {}", error.GetCString()).str());
      // error = process->Attach(m_options.attach_info);

    } else if (!args.coreFile.empty()) {
      dap.target.LoadCore(args.coreFile.data(), error);
    } else if (args.gdbRemotePort != LLDB_DAP_INVALID_PORT) {
      lldb::SBListener listener = dap.debugger.GetListener();

      // If the user hasn't provided the hostname property, default
      // localhost being used.
      std::string connect_url =
          llvm::formatv("connect://{0}:", args.gdbRemoteHostname);
      connect_url += std::to_string(args.gdbRemotePort);
      dap.target.ConnectRemote(listener, connect_url.c_str(), "gdb-remote",
                               error);

    } else if (!session) {
      // Attach by pid or process name.
      lldb::SBAttachInfo attach_info;
      if (args.pid != LLDB_INVALID_PROCESS_ID)
        attach_info.SetProcessID(args.pid);
      else if (!dap.configuration.program.empty())
        attach_info.SetExecutable(dap.configuration.program.data());
      attach_info.SetWaitForLaunch(args.waitFor, /*async=*/false);
      auto process = dap.target.Attach(attach_info, error);
      // If we attached by name then we were using the 'Dummy' target, ensure
      // we update to the real target.
      if (process.IsValid())
        dap.SetTarget(process.GetTarget());
    }

    if (error.Fail())
      return ToError(error);
  }

  const auto state_str =
      lldb::SBDebugger::StateAsCString(target.GetProcess().GetState());
  const auto id = target.GetProcess().GetProcessID();
  dap.SendOutput(
      OutputType::Console,
      llvm::formatv(" process state: {}, id {}", state_str, id).str());
  // Make sure the process is attached and stopped.
  error = dap.WaitForProcessToStop(args.configuration.timeout);
  if (error.Fail())
    return ToError(error);

  if (args.coreFile.empty() && !dap.target.GetProcess().IsValid())
    return make_error<DAPError>("failed to attach to process");

  dap.RunPostRunCommands();

  return Error::success();
}

} // namespace lldb_dap
