// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// tty.cpp -- terminal raw-mode setup and restore (rltty analogue).
//
// Puts the terminal into cbreak/no-echo so readline sees keystrokes as they
// are typed, while leaving signal generation (ISIG) on so C-c still works.
// No-op on non-ttys, so readline can be driven from a pipe in tests.

#include <termios.h>
#include <unistd.h>

#include "gnash/readline.hpp"
#include "gnash/readline_internal.hpp"

namespace gnash::readline {

namespace {
struct termios saved_tio;    // what deprep_terminal restores
struct termios prepped_tio;  // what prep_terminal set
bool tio_saved = false;
}  // namespace

void prep_terminal(int fd) {
  if (!isatty(fd)) return;
  if (tcgetattr(fd, &saved_tio) != 0) return;
  tio_saved = true;

  struct termios t = saved_tio;
  // Clear ICANON/ECHO for character-at-a-time input.  Also clear IEXTEN so the
  // driver stops intercepting the extended special characters -- notably VDSUSP
  // (C-y, delayed-suspend on BSD/macOS) and VLNEXT (C-v) -- and IXON so C-s/C-q
  // (flow control) reach readline for incremental search.  ISIG stays on, so
  // C-c and C-z still work for job control.
  t.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | IEXTEN));
  t.c_iflag &= static_cast<tcflag_t>(~(ICRNL | INLCR | IXON));
  t.c_cc[VMIN] = 1;
  t.c_cc[VTIME] = 0;
  prepped_tio = t;
  tcsetattr(fd, TCSADRAIN, &t);
}

// prep_terminal's modes do not necessarily hold until deprep_terminal.  A
// process that outlived the last foreground job -- a dev server's child doing
// a graceful shutdown after the C-c that killed its parent, say -- may still
// hold the tty open, and when it finally exits it writes back the cooked
// settings it saved at startup (bun and node both do).  That lands after the
// shell has taken the terminal back and readline has prepped it, and leaves
// readline blocked in read() on a canonical-mode tty: nothing arrives until
// Return (#712).  Called from the key-wait loop's idle tick: if the modes we
// set are gone, and the shell still owns the terminal, set them again.
void reassert_terminal(int fd) {
  if (!tio_saved) return;
  struct termios cur;
  if (tcgetattr(fd, &cur) != 0) return;
  if (cur.c_lflag == prepped_tio.c_lflag && cur.c_iflag == prepped_tio.c_iflag &&
      cur.c_cc[VMIN] == prepped_tio.c_cc[VMIN] && cur.c_cc[VTIME] == prepped_tio.c_cc[VTIME])
    return;
  if (tcgetpgrp(fd) != getpgrp()) return;  // someone else has the terminal
  tcsetattr(fd, TCSANOW, &prepped_tio);
}

void deprep_terminal(int fd) {
  if (!tio_saved) return;
  tcsetattr(fd, TCSADRAIN, &saved_tio);
  tio_saved = false;
}

bool terminal_prepped() { return tio_saved; }

}  // namespace gnash::readline
