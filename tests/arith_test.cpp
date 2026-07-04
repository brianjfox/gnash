// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// arith_test.cpp -- unit tests for the arithmetic evaluator.

#include <cstdio>

#include "gnash/core/shell.hpp"

namespace core = gnash::core;

static int failures = 0;

static void eq(const char *expr, long long want) {
  static core::Shell sh;
  bool ok = true;
  long long got = core::eval_arith(sh, expr, &ok);
  if (!ok || got != want) {
    std::fprintf(stderr, "FAIL eval(\"%s\") = %lld (ok=%d), wanted %lld\n", expr, got, ok, want);
    failures++;
  }
}

int main() {
  eq("2+3*4", 14);
  eq("(2+3)*4", 20);
  eq("2**10", 1024);
  eq("17 % 5", 2);
  eq("100 / 7", 14);
  eq("1 << 8", 256);
  eq("255 & 0x0f", 15);
  eq("0x10 + 010", 24);   // hex + octal
  eq("5 > 3", 1);
  eq("3 >= 4", 0);
  eq("5 == 5", 1);
  eq("2 != 2", 0);
  eq("1 && 0", 0);
  eq("1 || 0", 1);
  eq("!0", 1);
  eq("~0", -1);
  eq("-5 + 3", -2);
  eq("3 > 2 ? 10 : 20", 10);
  eq("0 ? 10 : 20", 20);
  eq("1, 2, 3", 3);
  eq("7 ^ 3", 4);

  // variables and assignment side effects share one Shell (static above).
  {
    core::Shell sh;
    bool ok = true;
    sh.set("x", "10");
    if (core::eval_arith(sh, "x * 2", &ok) != 20) { std::fprintf(stderr, "FAIL x*2\n"); failures++; }
    if (core::eval_arith(sh, "x += 5", &ok) != 15 || sh.get("x") != "15") { std::fprintf(stderr, "FAIL x+=5\n"); failures++; }
    if (core::eval_arith(sh, "x++", &ok) != 15 || sh.get("x") != "16") { std::fprintf(stderr, "FAIL x++\n"); failures++; }
    if (core::eval_arith(sh, "++x", &ok) != 17) { std::fprintf(stderr, "FAIL ++x\n"); failures++; }
    sh.set("y", "x + 4");  // recursive variable evaluation
    if (core::eval_arith(sh, "y * 2", &ok) != 42) { std::fprintf(stderr, "FAIL recursive y\n"); failures++; }
  }

  if (failures == 0) {
    std::printf("all arith tests passed\n");
    return 0;
  }
  std::fprintf(stderr, "%d arith test(s) failed\n", failures);
  return 1;
}
