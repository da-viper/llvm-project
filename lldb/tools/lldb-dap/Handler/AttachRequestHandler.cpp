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
#ifndef _WIN32
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

  struct cmsghdr *cmsg = nullptr;
  for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; CMSG_NXTHDR(&msg, cmsg)) {
    if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
      continue;

    int fd{};
    ::memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));

    // Confirm it's a socket
    int optval{};
    socklen_t optlen = sizeof(optval);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &optval, &optlen) < 0) {
      return llvm::createStringErrorV(
          "the received file descriptor {} is not a socket", fd);
      return -1;
    }

    // check for errors.
    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0)
      return llvm::createStringErrorV("socket has error: {}", strerror(err));

    return fd;
  }

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
  ::strncpy((char *)addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

  if (::connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    return llvm::errorCodeToError(llvm::errnoAsErrorCode());
  }

  llvm::scope_exit cleanup([&] { ::close(sock_fd); });

  return recv_fd(sock_fd);
}
#endif
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
    target = dap.CreateTarget(error, args.fdConnectionURI.empty());
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
      // Run the attach commands, after which we expect the debugger's selected
      // target to contain a valid and stopped process. Otherwise inform the
      // user that their command failed or the debugger is in an unexpected
      // state.
      if (llvm::Error err = dap.RunAttachCommands(args.attachCommands))
        return err;

      dap.SetTarget(dap.debugger.GetSelectedTarget());

      // Validate the attachCommand results.
      if (!dap.target.GetProcess().IsValid())
        return make_error<DAPError>(
            "attachCommands failed to attach to a process");

      // If the attach commands produced a non-live session (e.g. a core file
      // loaded via `target create --core`), report the session's stop reason
      // instead of trying to resume it, matching the behavior of the
      // `coreFile` attach key.
      if (!dap.target.GetProcess().IsLiveDebugSession()) {
        dap.stop_at_entry = true;
        dap.is_live_session = false;
      }
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
    if (!args.fdConnectionURI.empty()) {
      const auto &socket_path = args.fdConnectionURI;
      llvm::Expected<lldb::file_t> exp_fd = GetFDFromSocket(socket_path);
      if (llvm::Error error = exp_fd.takeError()) {
        return error;
      }

      const auto server_fd = *exp_fd;
      // TODO: Is this this needed as the internal write diables no sigpipe.
#ifdef __APPLE__
      const int flags = SO_NOSIGPIPE;
#else
      const int flags = 0;
#endif // __APPLE__
      int opt = 1;
      ::setsockopt(server_fd, SOL_SOCKET, flags, &opt, sizeof(opt));

      lldb::SBListener listener = dap.debugger.GetListener();
      const std::string connect_url = llvm::formatv("fd://{}", server_fd);
      lldb::SBProcess connect_process = target.ConnectRemote(listener, connect_url.c_str(),
                                                  "gdb-remote", error);
      m_connection_process = connect_process;
      if (error.Fail())
        return ToError(error);
      // NOTE: there is a weird setup flow.
      // the Proccess::Attach is not exposed through the SB-API so we use
      // SBTarget::Attach as it will use the underlying process for that target
      // to perform the attach.

      if (args.pid != LLDB_INVALID_PROCESS_ID)
        connect_process.RemoteAttachToProcessWithID(args.pid, error);
      else
        return llvm::createStringError("a process id is required to connect ");
      // lldb::SBAttachInfo attach_info;
      // attach_info.SetProcessID(args.pid);
      // // TODO: for some reason we are not able to attach by the name of a
      // process.
      // // because technically debugserver fd (in this case is actually a
      // SBPlatform not a process).
      // // and the process list is not exposed at this layer so for now only
      // support attaching using a
      // // pid.
      // // else if (!dap.configuration.program.empty())
      // //   attach_info.SetExecutable(dap.configuration.program.c_str());
      // // attach_info.SetWaitForLaunch(args.waitFor, /*async=*/false);

      // auto debug_process = target.Attach(attach_info, error);
      // if (debug_process.IsValid())
      //   dap.SetTarget(debug_process.GetTarget());
    }

    if (error.Fail())
      return ToError(error);
  }

  const char *const state_str =
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
