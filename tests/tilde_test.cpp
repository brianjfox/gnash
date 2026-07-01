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

static int failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                          \
    }                                                                      \
  } while (0)

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

  if (failures == 0) {
    std::printf("all tilde tests passed\n");
    return 0;
  }
  std::fprintf(stderr, "%d tilde test(s) failed\n", failures);
  return 1;
}
