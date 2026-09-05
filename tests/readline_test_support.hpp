// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// readline_test_support.hpp -- shared helpers for the readline unit tests.
//
// The readline tests drive readline() from an in-memory byte stream instead
// of a tty; this header provides that driver and the line-comparison check
// the tests build on it.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "readline/readline.h"

#include "testcheck.hpp"

namespace gnashtest {

// Feed INPUT as keystrokes; return the (malloc'd) line readline() produces,
// or nullptr on EOF.  Output goes to /dev/null.
inline char *run(const std::string &input) {
  FILE *f = std::tmpfile();
  if (input.size()) std::fwrite(input.data(), 1, input.size(), f);
  std::rewind(f);
  rl_instream = f;
  rl_outstream = std::fopen("/dev/null", "w");
  char *r = readline("");
  std::fclose(f);
  if (rl_outstream) std::fclose(rl_outstream);
  rl_instream = nullptr;
  rl_outstream = nullptr;
  return r;
}

// Assert that readline() over INPUT returns exactly WANT (freed afterwards).
inline void expect_line(const std::string &in, const char *want) {
  char *r = run(in);
  if (r == nullptr || std::strcmp(r, want) != 0) {
    std::fprintf(stderr, "FAIL run: got \"%s\", wanted \"%s\"\n", r ? r : "(null)", want);
    failures++;
  }
  std::free(r);
}

}  // namespace gnashtest
