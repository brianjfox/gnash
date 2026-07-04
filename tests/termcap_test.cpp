// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// termcap_test.cpp -- unit tests for the termcap library (built-in database).
//
// TERMCAP is cleared so the built-in entries are exercised deterministically.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "gnash/termcap.hpp"
#include "termcap.h"

static int failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                          \
    }                                                                      \
  } while (0)

static bool str_is(const char *cap, const char *want) {
  char *s = tgetstr(cap, nullptr);
  bool ok = s && std::strcmp(s, want) == 0;
  if (!ok)
    std::fprintf(stderr, "FAIL tgetstr(%s): got \"%s\", wanted \"%s\"\n", cap,
                 s ? s : "(null)", want);
  std::free(s);
  return ok;
}

int main() {
  unsetenv("TERMCAP");

  // Known terminal from the built-in DB.
  CHECK(tgetent(nullptr, "xterm") == 1);
  CHECK(tgetnum("co") == 80);
  CHECK(tgetnum("li") == 24);
  CHECK(tgetflag("am") == 1);
  CHECK(tgetflag("zz") == 0);        // absent flag
  CHECK(tgetnum("zz") == -1);        // absent number

  CHECK(str_is("up", "\033[A"));     // cursor up
  CHECK(str_is("nd", "\033[C"));     // cursor right
  CHECK(str_is("le", "\010"));       // backspace (^H)
  CHECK(str_is("cr", "\015"));       // carriage return (^M)
  CHECK(str_is("cl", "\033[H\033[2J"));  // clear screen
  CHECK(str_is("ce", "\033[K"));     // clear to end of line
  CHECK(str_is("ku", "\033OA"));     // up-arrow key

  // Alias resolves to the same entry.
  CHECK(tgetent(nullptr, "xterm-256color") == 1);
  CHECK(tgetnum("co") == 80);

  // Cursor addressing via tgoto: cm = "\033[%i%d;%dH", tgoto(cm, col, row).
  char *cm = tgetstr("cm", nullptr);
  CHECK(cm != nullptr);
  if (cm) {
    char *seq = tgoto(cm, 9, 4);  // hpos=col 9, vpos=row 4 -> row;col 1-based
    CHECK(seq && std::strcmp(seq, "\033[5;10H") == 0);
    if (seq && std::strcmp(seq, "\033[5;10H") != 0)
      std::fprintf(stderr, "tgoto -> \"%s\"\n", seq);
    std::free(cm);
  }

  // Unknown terminal: not found.
  CHECK(tgetent(nullptr, "no_such_term_xyz") == 0);

  // vt100 differs from xterm in some caps.
  CHECK(tgetent(nullptr, "vt100") == 1);
  CHECK(str_is("do", "\012"));  // down = ^J on vt100

  // C++ wrapper.
  CHECK(gnash::termcap::load("xterm"));
  CHECK(gnash::termcap::num("co") == 80);
  CHECK(gnash::termcap::flag("am"));
  auto ce = gnash::termcap::str("ce");
  CHECK(ce && *ce == std::string("\033[K"));

  if (failures == 0) {
    std::printf("all termcap tests passed\n");
    return 0;
  }
  std::fprintf(stderr, "%d termcap test(s) failed\n", failures);
  return 1;
}
