// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// readline_bind_test.cpp -- inputrc parsing/binding and vi mode.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "readline/readline.h"

#include "readline_test_support.hpp"

using gnashtest::failures;

int main() {
  rl_initialize();

  // ---- rl_named_function ----
  CHECK(rl_named_function("beginning-of-line") == rl_beg_of_line);
  CHECK(rl_named_function("kill-word") == rl_kill_word);
  CHECK(rl_named_function("no-such-function") == nullptr);

  // ---- rl_parse_and_bind: bind C-o to end-of-line ----
  {
    char line[] = "\"\\C-o\": end-of-line";
    CHECK(rl_parse_and_bind(line) == 0);
  }
  // "ab" C-a (to start) then C-o (our binding -> end-of-line) then 'Z' -> "abZ"
  gnashtest::expect_line("ab\x01\x0f"
                         "Z\n",
                         "abZ");

  // ---- bind a multi-key sequence: ESC [ Z (back-tab) -> beginning-of-line ----
  {
    char line[] = "\"\\e[Z\": beginning-of-line";
    CHECK(rl_parse_and_bind(line) == 0);
  }
  gnashtest::expect_line("abc\x1b[Z"
                         "X\n",
                         "Xabc");

  // ---- vi mode via `set editing-mode vi` ----
  {
    char line[] = "set editing-mode vi";
    rl_parse_and_bind(line);
  }
  CHECK(rl_editing_mode == 0);

  // In vi: type "hello", ESC (command mode, cursor on 'o'), '0' (line start),
  // 'i' (insert), 'X', RET -> "Xhello".
  gnashtest::expect_line("hello\x1b"
                         "0iX\r",
                         "Xhello");

  // vi: "world", ESC, 'x' deletes char under cursor ('d'? cursor on last 'd'),
  // -> "worl", RET.
  gnashtest::expect_line("world\x1b"
                         "x\r",
                         "worl");

  // Back to emacs for any later users.
  {
    char line[] = "set editing-mode emacs";
    rl_parse_and_bind(line);
  }
  CHECK(rl_editing_mode == 1);

  return gnashtest::finish("bind/vi");
}
