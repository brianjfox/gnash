// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// parser_test.cpp -- parser + AST canonical rendering, and error detection.

#include <cstdio>
#include <string>

#include "gnash/core/parser.hpp"

namespace core = gnash::core;

static int failures = 0;

// Parse `in`; assert it succeeds and renders to `want`.
static void ok(const std::string &in, const char *want) {
  core::ParseResult r = core::parse(in);
  if (!r.ok) {
    std::fprintf(stderr, "FAIL parse(%s): unexpected error: %s\n", in.c_str(), r.error.c_str());
    failures++;
    return;
  }
  std::string got = core::to_string(r.command.get());
  if (got != want) {
    std::fprintf(stderr, "FAIL parse(%s): got \"%s\", wanted \"%s\"\n", in.c_str(),
                 got.c_str(), want);
    failures++;
  }
}

// Parse `in` with the given regular/global/suffix alias tables; assert the
// expanded result renders to `want`.
static void alias_ok(const std::string &in,
                     const std::map<std::string, std::string> &reg,
                     const std::map<std::string, std::string> &glob,
                     const std::map<std::string, std::string> &suf, const char *want) {
  core::ParseResult r = core::parse_with_aliases(in, reg, glob, suf);
  if (!r.ok) {
    std::fprintf(stderr, "FAIL alias parse(%s): error: %s\n", in.c_str(), r.error.c_str());
    failures++;
    return;
  }
  std::string got = core::to_string(r.command.get());
  if (got != want) {
    std::fprintf(stderr, "FAIL alias parse(%s): got \"%s\", wanted \"%s\"\n", in.c_str(),
                 got.c_str(), want);
    failures++;
  }
}

// Assert that `in` is a syntax error.
static void bad(const std::string &in) {
  core::ParseResult r = core::parse(in);
  if (r.ok) {
    std::fprintf(stderr, "FAIL parse(%s): expected syntax error, got \"%s\"\n", in.c_str(),
                 core::to_string(r.command.get()).c_str());
    failures++;
  }
}

int main() {
  // simple + lists + pipelines
  ok("echo hi", "echo hi");
  ok("a && b", "a && b");
  ok("a || b && c", "a || b && c");
  ok("a | b | c", "a | b | c");
  ok("a; b", "a; b");
  ok("a; b;", "a; b");             // trailing separator dropped
  ok("a &", "a &");
  ok("a & b", "a & b");
  ok("! foo", "! foo");
  ok("a |& b", "a 2>&1 | b");   // |& adds 2>&1 to the left command

  // redirections
  ok("ls > out", "ls >out");
  ok("cat < in > out", "cat <in >out");
  ok("ls 2>&1", "ls 2>&1");
  ok("echo hi >> log", "echo hi >>log");

  // compound commands
  ok("( a; b )", "(a; b)");
  ok("{ a; b; }", "{ a; b; }");
  ok("if a; then b; fi", "if a; then b; fi");
  ok("if a; then b; else c; fi", "if a; then b; else c; fi");
  ok("if a; then b; elif c; then d; fi", "if a; then b; else if c; then d; fi; fi");
  ok("while a; do b; done", "while a; do b; done");
  ok("until a; do b; done", "until a; do b; done");
  ok("for x in a b c; do echo $x; done", "for x in a b c; do echo $x; done");
  ok("for x; do echo $x; done", "for x; do echo $x; done");
  ok("case $x in a) echo 1;; b|c) echo 2;; esac",
     "case $x in a) echo 1 ;; b | c) echo 2 ;; esac");
  ok("f() { echo hi; }", "f () { echo hi; }");
  ok("{ a; } > out", "{ a; } >out");

  // conditional [[ ]]
  ok("[[ -f foo ]]", "[[ -f foo ]]");
  ok("[[ $x == a* ]]", "[[ $x == a* ]]");
  ok("[[ -f a && -d b ]]", "[[ -f a && -d b ]]");
  ok("[[ ! -z $x ]]", "[[ ! -z $x ]]");
  ok("[[ ( -f a || -f b ) && -d c ]]", "[[ ( -f a || -f b ) && -d c ]]");
  ok("[[ $x =~ ^ab.*$ ]]", "[[ $x =~ ^ab.*$ ]]");

  // arithmetic (( )) and arithmetic for
  ok("(( x + 1 ))", "((x + 1))");
  ok("(( i = 0 ))", "((i = 0))");
  // arithmetic sections are rendered faithfully to the source spacing, so a
  // glued operator like `<' (which the lexer splits) is re-glued, not spaced.
  ok("for ((i=0; i<10; i++)); do echo $i; done", "for ((i=0; i<10; i++)); do echo $i; done");
  // `((cmd); cmd)` is not arithmetic -> falls back to nested subshells
  ok("(( echo hi ); echo bye )", "((echo hi); echo bye)");

  // select, coproc, array assignment
  ok("select x in a b; do echo $x; done", "select x in a b; do echo $x; done");
  ok("coproc co { read line; }", "coproc co { read line; }");
  ok("a=(1 2 3)", "a=(1 2 3)");
  ok("x=1 a=(1 2) cmd", "x=1 a=(1 2) cmd");

  // errors
  bad("[[ ]]");                // empty conditional
  bad("[[ -f ]]");             // missing operand
  bad("[[ a ==");             // missing rhs + ]]
  bad("[[ a ]");              // missing second ]
  bad("echo ${unterminated");  // unterminated ${
  bad("echo $(cmd");           // unterminated $(
  bad("echo 'unterminated");   // unterminated quote
  bad("if a; then b");         // missing fi
  bad("while a; do b");        // missing done
  bad("done");                 // misplaced keyword
  bad("a |");                  // dangling pipe
  bad("( a");                  // unclosed subshell
  bad("case x in a) b;;");     // missing esac
  bad("for; do x; done");      // missing variable

  // -- alias expansion: regular (command position), zsh global (any position),
  //    zsh suffix (file.ext -> cmd file.ext) --
  alias_ok("ll -a", {{"ll", "ls -l"}}, {}, {}, "ls -l -a");            // regular
  alias_ok("echo a P b", {}, {{"P", "| wc"}}, {}, "echo a | wc b");     // global, mid-command
  alias_ok("cmd G", {}, {{"G", "| grep x"}}, {}, "cmd | grep x");       // global at end
  alias_ok("echo P", {}, {}, {}, "echo P");                            // no tables: unchanged
  alias_ok("readme.md", {}, {}, {{"md", "less"}}, "less readme.md");    // suffix
  alias_ok("a.txt b.txt", {}, {}, {{"txt", "cat"}}, "cat a.txt b.txt"); // suffix only in cmd pos

  if (failures == 0) {
    std::printf("all parser tests passed\n");
    return 0;
  }
  std::fprintf(stderr, "%d parser test(s) failed\n", failures);
  return 1;
}
