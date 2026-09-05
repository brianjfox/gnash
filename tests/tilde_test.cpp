// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// tilde_test.cpp -- unit tests for the tilde library.
//
// HOME is pinned so expansions are deterministic and independent of the
// invoking user.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "gnash/tilde.hpp"
#include "readline/tilde.h"

#include "testcheck.hpp"

using gnashtest::failures;

static void expect(const char *in, const char *want) {
  char *r = tilde_expand(in);
  if (r == nullptr || std::strcmp(r, want) != 0) {
    std::fprintf(stderr, "FAIL tilde_expand(\"%s\"): got \"%s\", wanted \"%s\"\n",
                 in, r ? r : "(null)", want);
    failures++;
  }
  std::free(r);
}

int main() {
  setenv("HOME", "/test/home", 1);

  expect("~", "/test/home");
  expect("~/foo", "/test/home/foo");
  expect("~/a/b/c", "/test/home/a/b/c");
  expect("no tilde here", "no tilde here");
  expect("/absolute/path", "/absolute/path");

  // Unknown user: left untouched.
  expect("~no_such_user_xyz/x", "~no_such_user_xyz/x");

  // Additional prefix (space + tilde) expands mid-string by default.
  expect("echo ~/x", "echo /test/home/x");

  // tilde_expand_word on a bare word.
  char *w = tilde_expand_word("~/bar");
  CHECK(w && std::strcmp(w, "/test/home/bar") == 0);
  std::free(w);

  // C++ wrapper.
  CHECK(gnash::tilde::expand("~/z") == "/test/home/z");
  CHECK(gnash::tilde::expand_word("~") == "/test/home");

  return gnashtest::finish("tilde");
}
