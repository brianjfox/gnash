// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// input.cpp -- reading keys from rl_instream.
//
// For a terminal we read a byte at a time with read(2), waiting via select(2)
// so that while the user is idle at the prompt we can periodically run
// rl_event_hook (used for asynchronous notices such as background-job
// completion) without needing input first.  For a non-tty (a pipe, as in the
// test harness) we keep the simple blocking stdio path.

#include <cerrno>
#include <cstdio>
#include <sys/select.h>
#include <unistd.h>

#include "gnash/readline_internal.hpp"
#include "readline/readline.h"

namespace {
// How long to block in select() before giving the event hook a turn.
constexpr long kIdlePollUsec = 100000;  // 100ms -- imperceptible, cheap when idle
}  // namespace

extern "C" int rl_read_key(void) {
  if (rl_instream == nullptr) rl_instream = stdin;
  int fd = fileno(rl_instream);

  if (!isatty(fd)) {
    return std::getc(rl_instream);  // 0..255 or EOF (-1)
  }

  for (;;) {
    if (gnash::readline::rl_sigint_flag) return EOF;  // C-c pending: abort read
    if (rl_event_hook) {
      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(fd, &rfds);
      struct timeval tv;
      tv.tv_sec = 0;
      tv.tv_usec = kIdlePollUsec;
      int r = select(fd + 1, &rfds, nullptr, nullptr, &tv);
      if (gnash::readline::rl_sigint_flag) return EOF;  // C-c: let the loop abort
      if (r == 0) {          // idle: let the hook run, then keep waiting
        rl_event_hook();
        continue;
      }
      if (r < 0) {
        if (errno == EINTR) { rl_event_hook(); continue; }
        return EOF;
      }
      // fd is readable -- fall through to read the byte.
    }
    unsigned char ch;
    ssize_t n = read(fd, &ch, 1);
    if (n == 1) return ch;
    if (n == 0) return EOF;
    if (errno == EINTR) continue;  // signal (incl. C-c): loop rechecks the flag
    return EOF;
  }
}
