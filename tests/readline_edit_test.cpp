// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// readline_edit_test.cpp -- unit tests for the Readline editing core.
//
// Drives the bindable commands directly (no tty) and checks the resulting line
// buffer and cursor, verifying emacs-style editing semantics.

#include <cstdio>
#include <string>

#include "gnash/readline.hpp"
#include "readline/readline.h"

namespace rl = gnash::readline;

static int failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                          \
    }                                                                      \
  } while (0)

static void check_line(const char *want) {
  if (rl::line() != want) {
    std::fprintf(stderr, "FAIL line: got \"%s\", wanted \"%s\"\n",
                 rl::line().c_str(), want);
    failures++;
  }
}

// Start each scenario from a clean line and kill state.
static void begin(const char *text) {
  rl_reset_last_command();
  rl::set_line(text);
}

int main() {
  // -- self-insert ---------------------------------------------------------
  begin("");
  rl_insert(1, 'h');
  rl_insert(1, 'i');
  rl_insert(3, '!');
  check_line("hi!!!");
  CHECK(rl::point() == 5);

  // -- word / line motion --------------------------------------------------
  begin("hello world");
  CHECK(rl::point() == 11);
  rl_backward_word(1, 0);
  CHECK(rl::point() == 6);
  rl_backward_word(1, 0);
  CHECK(rl::point() == 0);
  rl_forward_word(1, 0);
  CHECK(rl::point() == 5);
  rl_end_of_line(0, 0);
  CHECK(rl::point() == 11);
  rl_beg_of_line(0, 0);
  CHECK(rl::point() == 0);

  // -- rubout / delete -----------------------------------------------------
  begin("abcd");
  rl_rubout(1, 0);           // delete 'd'
  check_line("abc");
  CHECK(rl::point() == 3);
  rl::set_point(0);
  rl_delete(1, 0);           // delete 'a'
  check_line("bc");
  CHECK(rl::point() == 0);

  // -- kill line + yank ----------------------------------------------------
  begin("hello world");
  rl::set_point(5);
  rl_kill_line(0, 0);        // kill " world"
  check_line("hello");
  rl_yank(0, 0);             // paste it back
  check_line("hello world");
  CHECK(rl::point() == 11);
  CHECK(rl_mark == 5);

  // -- backward kill accumulation ------------------------------------------
  begin("foo bar");
  rl_backward_kill_word(1, 0);   // kill "bar"
  check_line("foo ");
  rl_backward_kill_word(1, 0);   // kill "foo ", prepended -> "foo bar"
  check_line("");
  rl_yank(0, 0);
  check_line("foo bar");

  // -- unix-word-rubout ----------------------------------------------------
  begin("foo   bar");
  rl_unix_word_rubout(0, 0);
  check_line("foo   ");

  // -- transpose chars -----------------------------------------------------
  begin("ab");                   // point at end
  rl_transpose_chars(0, 0);
  check_line("ba");
  CHECK(rl::point() == 2);

  begin("abc");
  rl::set_point(1);
  rl_transpose_chars(0, 0);      // swap around point, advance
  check_line("bac");
  CHECK(rl::point() == 2);

  // -- case commands -------------------------------------------------------
  begin("hello world");
  rl::set_point(0);
  rl_upcase_word(1, 0);
  check_line("HELLO world");
  CHECK(rl::point() == 5);
  rl_capitalize_word(1, 0);
  check_line("HELLO World");

  begin("HELLO");
  rl::set_point(0);
  rl_downcase_word(1, 0);
  check_line("hello");

  if (failures == 0) {
    std::printf("all readline editing tests passed\n");
    return 0;
  }
  std::fprintf(stderr, "%d readline editing test(s) failed\n", failures);
  return 1;
}
