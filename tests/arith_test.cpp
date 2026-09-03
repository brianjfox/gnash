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

// The expression must FAIL to evaluate (a bash arithmetic error).
static void bad(const char *expr) {
  static core::Shell sh;
  bool ok = true;
  long long got = core::eval_arith(sh, expr, &ok);
  if (ok) {
    std::fprintf(stderr, "FAIL eval(\"%s\") = %lld, wanted an error\n", expr, got);
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
  eq("10#42", 42);        // bash base#digits notation
  eq("16#ff", 255);
  eq("2#101", 5);
  eq("8#17", 15);
  eq("36#z", 35);         // base <= 36: letters case-insensitive
  eq("64#Z", 61);         // base > 36: uppercase is 36-61
  eq("64#_", 63);         // ... then @ (62) and _ (63)
  eq("16#1f + 1", 32);    // base#digits inside an expression
  eq("10#00042", 42);     // leading zeros are decimal, not octal
  // The base is scanned like any literal (bash's strlong): a leading `0'
  // makes it OCTAL, so `08#1' fails on the `8' rather than reading base 8.
  bad("08#1");
  bad("08#7");
  bad("010#1");            // base already found (octal) when `#' arrives
  bad("0x10#f");
  bad("0#1");
  bad("00#1");
  eq("8#1", 1);
  eq("0x1F", 31);
  eq("017", 15);
  eq("0", 0);
  eq("0x", 0);             // empty hex run is 0 (non-strict bash)
  bad("1#0");              // invalid arithmetic base
  bad("65#1");
  bad("2#2");              // value too great for base
  bad("2#1x");
  bad("8#8");
  bad("10#a");
  bad("08");
  bad("0xg");
  bad("2#");               // `#' must be followed by a digit character
  bad("2##1");
  bad("2#1#1");            // a second `#' is an invalid number
  bad("1e3");
  eq("37#A", 36);          // base > 36: uppercase starts at 36
  eq("36#A", 10);
  eq("2#1 + 2#1", 2);
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
  // The true branch of `?:' is a full comma expression in bash (expcond
  // parses it with EXP_HIGHEST); the false branch binds tighter than `,'.
  eq("1 ? 2 , 3 : 4", 3);
  eq("0 ? 2 , 3 : 4", 4);
  eq("1 ? 2 : 3 , 4", 4);
  eq("1 ? 2 , 3 : 4 , 5", 5);
  eq("1 ? 2 ? 3 , 4 : 5 : 6", 4);
  eq("0 ? 1 / 0 , 2 : 3", 3);
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
