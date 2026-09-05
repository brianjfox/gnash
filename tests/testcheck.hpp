// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// testcheck.hpp -- shared scaffolding for the standalone unit-test
// executables in this directory.
//
// Each test is a small program that counts failures and exits nonzero if
// any check failed.  This header holds the pieces every test shares so they
// are not re-pasted file by file:
//   - failures: the shared failure counter (each test TU imports it with a
//               `using gnashtest::failures;' right after the include)
//   - CHECK:    assert a condition, printing file:line and counting it
//   - count_of: count non-overlapping occurrences of a needle in a string
//   - finish:   print the pass/fail summary and return the exit status

#pragma once

#include <cstdio>
#include <string>

namespace gnashtest {

// The test's failure counter.
inline int failures = 0;

// Print the per-test summary and return the process exit status.  LABEL is
// the test's noun: finish("tilde") prints "all tilde tests passed" or
// "<n> tilde test(s) failed".
inline int finish(const char *label) {
  if (failures == 0) {
    std::printf("all %s tests passed\n", label);
    return 0;
  }
  std::fprintf(stderr, "%d %s test(s) failed\n", failures, label);
  return 1;
}

// Count non-overlapping occurrences of NEEDLE in HAY.
inline size_t count_of(const std::string &hay, const std::string &needle) {
  size_t n = 0;
  for (size_t at = hay.find(needle); at != std::string::npos;
       at = hay.find(needle, at + needle.size()))
    n++;
  return n;
}

}  // namespace gnashtest

// Report a failed condition with file:line context and count it in the
// shared `failures' counter (imported into the test's global scope).
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                         \
    }                                                                     \
  } while (0)
