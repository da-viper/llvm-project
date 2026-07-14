//===-- ConnectRemoteFDRequestHandler.cpp ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DAP.h"
#include "Handler/RequestHandler.h"
#include "LLDBUtils.h"
#include "Protocol/ProtocolRequests.h"
#include "lldb/API/SBListener.h"
#include "lldb/lldb-types.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Error.h"
#include <sys/socket.h>
#include <sys/un.h>

using namespace lldb_dap::protocol;

static llvm::Expected<lldb::file_t> recv_fd(int sock) {
  char dummy{};
  struct iovec iov = {.iov_base = &dummy, .iov_len = 1};

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
  if (recvmsg(sock, &msg, flags) < 0)
    return llvm::errorCodeToError(llvm::errnoAsErrorCode());

  struct cmsghdr *cmsg = NULL;
  for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; CMSG_NXTHDR(&msg, cmsg)) {
    if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
      continue;

    int fd{};
    memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
    return fd;
  }

  // TODO: fprintf(stderr, "no file descriptor in received message\n");
  return llvm::createStringError("failed to get the file descriptor");
}

static llvm::Expected<lldb::file_t> GetFDFromSocket(const String &socket_path) {
  const char *sock_path = socket_path.c_str();

  const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listener < 0) {
    return llvm::errorCodeToError(llvm::errnoAsErrorCode());
  }

  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

  // A leftover socket file from a previous run makes bind() fail with
  // EADDRINUSE.
  unlink(sock_path);
  if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    return llvm::errorCodeToError(llvm::errnoAsErrorCode());
  }
  if (listen(listener, 1) < 0) {
    return llvm::errorCodeToError(llvm::errnoAsErrorCode());
  }
  //   printf("[receiver] listening on %s\n", sock_path);

  const int conn = accept(listener, NULL, NULL);
  if (conn < 0) {
    return llvm::errorCodeToError(llvm::errnoAsErrorCode());
  }

  llvm::scope_exit cleanup([&] {
    close(conn);
    close(listener);
    unlink(sock_path);
  });
  return recv_fd(conn);
}

namespace lldb_dap {
llvm::Error
ConnectFDRemoteRequestHandler::Run(const ConnectRemoteFDArguments &args) const {
  //   lldb::file_t received_fd = -1;
  //   if (received_fd < 0) {
  //     return llvm::make_error<DAPError>(
  //         "no handle received from the socket connection");
  //   }

#ifdef __APPLE__ // SO_NOSIGPIPE is not available on linux. TODO: add FreeBSD
  int opt = 1;
  ::setsockopt(received_fd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif

  //   const auto &connect_url = args.connection_url;
  llvm::Expected<lldb::file_t> exp_fd = GetFDFromSocket(args.connection_url);
  if (llvm::Error error = exp_fd.takeError()) {
    return error;
  }

  const std::string connect_url = llvm::formatv("fd://{}", *exp_fd);
  lldb::SBListener listener = dap.debugger.GetListener();
  lldb::SBError error;
  auto process = dap.target.ConnectRemote(listener, connect_url.c_str(),
                                          args.protocol.c_str(), error);

  return ToError(error);
  //   if (error.Fail()) {
  //     ::close(received_fd);
  //     return ToError(error);
  //   }
  //   return llvm::Error::success();
}
} // namespace lldb_dap