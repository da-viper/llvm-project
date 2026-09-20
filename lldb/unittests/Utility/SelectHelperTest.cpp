//===-- SelectHelperTest.cpp ----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Utility/SelectHelper.h"
#include "lldb/Utility/Status.h"
#include "gtest/gtest.h"

#include <cerrno>
#include <chrono>

#if !defined(_WIN32)
#include <sys/resource.h>
#include <unistd.h>
#endif

using namespace std::chrono_literals;

namespace {

#if !defined(_WIN32)
// Owning pair of pipe fds, closed on destruction.
struct Pipe {
  int read_fd = -1;
  int write_fd = -1;

  Pipe() {
    int fds[2] = {-1, -1};
    if (::pipe(fds) == 0) {
      read_fd = fds[0];
      write_fd = fds[1];
    }
  }
  ~Pipe() {
    if (read_fd >= 0)
      ::close(read_fd);
    if (write_fd >= 0)
      ::close(write_fd);
  }
  Pipe(const Pipe &) = delete;
  Pipe &operator=(const Pipe &) = delete;
};

TEST(SelectHelperTest, TimeoutReturnsETIMEDOUT) {
  Pipe p;
  ASSERT_GE(p.read_fd, 0);

  SelectHelper sh;
  sh.FDSetRead(p.read_fd);
  sh.SetTimeout(1ms);

  lldb_private::Status status = sh.Select();
  EXPECT_EQ(status.GetError(), ETIMEDOUT);
  EXPECT_FALSE(sh.FDIsSetRead(p.read_fd));
}

TEST(SelectHelperTest, ReadWakesOnPipeData) {
  Pipe p;
  ASSERT_GE(p.read_fd, 0);
  const char byte = 'x';
  ASSERT_EQ(::write(p.write_fd, &byte, 1), 1);

  SelectHelper sh;
  sh.FDSetRead(p.read_fd);
  sh.SetTimeout(1s);

  lldb_private::Status status = sh.Select();
  EXPECT_FALSE(status.Fail());
  EXPECT_TRUE(sh.FDIsSetRead(p.read_fd));
}

TEST(SelectHelperTest, WriteIsSetOnEmptyPipe) {
  Pipe p;
  ASSERT_GE(p.write_fd, 0);

  SelectHelper sh;
  sh.FDSetWrite(p.write_fd);
  sh.SetTimeout(1s);

  lldb_private::Status status = sh.Select();
  EXPECT_FALSE(status.Fail());
  EXPECT_TRUE(sh.FDIsSetWrite(p.write_fd));
}

TEST(SelectHelperTest, PeerCloseIsReadable) {
  Pipe p;
  ASSERT_GE(p.read_fd, 0);
  ASSERT_GE(p.write_fd, 0);
  ::close(p.write_fd);
  p.write_fd = -1; // avoid double-close in dtor

  SelectHelper sh;
  sh.FDSetRead(p.read_fd);
  sh.SetTimeout(1s);

  lldb_private::Status status = sh.Select();
  EXPECT_FALSE(status.Fail());
  // Reading from a hung-up peer surfaces as readable — select() semantics
  // preserved on the poll() path via POLLHUP folding.
  EXPECT_TRUE(sh.FDIsSetRead(p.read_fd));
}

TEST(SelectHelperTest, EmptyMapReturnsError) {
  SelectHelper sh;
  sh.SetTimeout(1ms);
  lldb_private::Status status = sh.Select();
  EXPECT_TRUE(status.Fail());
}

#if !defined(__APPLE__)
// This is the regression test for the FD_SETSIZE ceiling that made
// lldb-dap in server mode fail after ~1000 concurrent sessions on Linux.
// Windows uses select() with a count-based FD_SETSIZE (irrelevant here),
// and Apple's select() supports unlimited fds via _DARWIN_UNLIMITED_SELECT.
TEST(SelectHelperTest, HighFdSucceeds) {
  // Try to raise the fd ceiling. If we can't (e.g. sandboxed CI), skip.
  struct rlimit rl;
  if (::getrlimit(RLIMIT_NOFILE, &rl) != 0)
    GTEST_SKIP() << "getrlimit failed";
  if (rl.rlim_cur < 4096) {
    rl.rlim_cur = std::min<rlim_t>(rl.rlim_max, 4096);
    if (::setrlimit(RLIMIT_NOFILE, &rl) != 0)
      GTEST_SKIP() << "cannot raise RLIMIT_NOFILE";
  }

  Pipe p;
  ASSERT_GE(p.read_fd, 0);
  ASSERT_GE(p.write_fd, 0);

  // dup2 the read end onto a high fd number (>= FD_SETSIZE on Linux).
  const int high_fd = 2000;
  const int dup_result = ::dup2(p.read_fd, high_fd);
  if (dup_result != high_fd)
    GTEST_SKIP() << "dup2 to high fd failed";

  const char byte = 'y';
  ASSERT_EQ(::write(p.write_fd, &byte, 1), 1);

  SelectHelper sh;
  sh.FDSetRead(high_fd);
  sh.SetTimeout(1s);

  lldb_private::Status status = sh.Select();
  EXPECT_FALSE(status.Fail()) << status.AsCString();
  EXPECT_TRUE(sh.FDIsSetRead(high_fd));

  ::close(high_fd);
}
#endif // !defined(__APPLE__)

#endif // !defined(_WIN32)

} // namespace
