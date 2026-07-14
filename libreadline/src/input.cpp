// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// input.cpp -- reading keys from rl_instream.
//
// For a terminal we read a byte at a time with read(2), waiting via select(2)
// so that while the user is idle at the prompt we can periodically run
// rl_event_hook (used for asynchronous notices such as background-job
// completion) without needing input first.  For a non-tty (a pipe, as in the
// test harness) we keep the simple blocking stdio path.
//
// A pushback queue sits in front of the real input: keyboard-macro replay
// (C-x e) and vi redo (`.') stuff their recorded keys here, and rl_read_key
// drains it before touching the fd.  Two optional taps record keys as they
// are read: one for macro definition (C-x (), one for capturing the keys of
// a vi change command so `.' can replay them.

#include <cerrno>
#include <cstdio>
#include <deque>
#include <string>
#include <sys/select.h>
#include <unistd.h>

#include "gnash/readline_internal.hpp"
#include "readline/readline.h"

namespace {
// How long to block in select() before giving the event hook a turn.
constexpr long kIdlePollUsec = 100000;  // 100ms -- imperceptible, cheap when idle

std::deque<unsigned char> pending;  // stuffed keys, consumed before the fd
}  // namespace

namespace gnash::readline {

bool macro_recording = false;
std::string macro_keys;       // keys recorded since C-x (
bool redo_capturing = false;
std::string redo_capture;     // keys of the vi change command in progress
std::string redo_keys;        // keys of the last completed vi change command

void stuff_input(const std::string &keys) {
  for (char c : keys) pending.push_back(static_cast<unsigned char>(c));
}

// True if a key can be read without blocking longer than TIMEOUT_MS: either
// the pushback queue is non-empty or the input fd becomes readable in time.
bool input_pending(int timeout_ms) {
  if (!pending.empty()) return true;
  if (rl_instream == nullptr) rl_instream = stdin;
  int fd = fileno(rl_instream);
  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(fd, &rfds);
  struct timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  return select(fd + 1, &rfds, nullptr, nullptr, &tv) > 0;
}

}  // namespace gnash::readline

namespace {

// Read one key from the pushback queue or the underlying stream.
int read_key_raw() {
  if (!pending.empty()) {
    int c = pending.front();
    pending.pop_front();
    return c;
  }

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

}  // namespace

extern "C" int rl_read_key(void) {
  int c = read_key_raw();
  if (c != EOF) {
    if (gnash::readline::macro_recording)
      gnash::readline::macro_keys += static_cast<char>(c);
    if (gnash::readline::redo_capturing)
      gnash::readline::redo_capture += static_cast<char>(c);
  }
  return c;
}
