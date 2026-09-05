// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// lexer_test.cpp -- shell tokenizer.

#include <cstdio>
#include <string>

#include "gnash/core/lexer.hpp"

#include "testcheck.hpp"

namespace core = gnash::core;

using gnashtest::failures;

// Render the token stream compactly: word/io text verbatim, operators by name,
// joined with spaces (EOF omitted).
static std::string render(const std::string &in) {
  std::string out;
  for (const core::Token &t : core::tokenize(in)) {
    if (t.type == core::Tok::Eof) break;
    if (!out.empty()) out += ' ';
    if (t.type == core::Tok::Word || t.type == core::Tok::IoNumber)
      out += t.text;
    else if (t.type == core::Tok::Newline)
      out += "<NL>";
    else
      out += core::tok_name(t.type);
  }
  return out;
}

static void expect(const std::string &in, const char *want) {
  std::string got = render(in);
  if (got != want) {
    std::fprintf(stderr, "FAIL tokenize(%s): got \"%s\", wanted \"%s\"\n", in.c_str(),
                 got.c_str(), want);
    failures++;
  }
}

int main() {
  expect("echo hello world", "echo hello world");
  expect("a && b || c", "a && b || c");
  expect("a|b", "a | b");
  expect("foo;bar", "foo ; bar");
  expect("a & b", "a & b");
  expect("ls > out 2>&1", "ls > out 2 >& 1");
  expect("cat < in >> out", "cat < in >> out");
  expect("echo 'a b' \"c d\"", "echo 'a b' \"c d\"");
  expect("echo $(date +%s) x", "echo $(date +%s) x");
  expect("echo ${HOME}/bin", "echo ${HOME}/bin");
  expect("echo \"$(echo \"nested\")\"", "echo \"$(echo \"nested\")\"");
  expect("x=1 echo hi", "x=1 echo hi");
  expect("echo hi # trailing comment", "echo hi");
  expect("(a; b)", "( a ; b )");
  expect("{ a; }", "{ a ; }");
  expect("a |& b", "a |& b");
  expect("case x in a) b;; esac", "case x in a ) b ;; esac");
  // Inside `$((' a `#' is never a comment (bash scans the span with P_ARITH):
  // the word ends at the matching `))', and the rest is still tokenized.
  expect("echo $(( $(echo 1)#1 ))\nnext", "echo $(( $(echo 1)#1 )) <NL> next");
  expect("echo $(( 2#101 )); y\nz", "echo $(( 2#101 )) ; y <NL> z");
  expect("echo $(echo a #c\n) z", "echo $(echo a #c\n) z");  // `$(cmd)' keeps comments

  // Here-document body is collected and attached to the delimiter word.
  {
    auto toks = core::tokenize("cat <<EOF\nline1\nline2\nEOF\n");
    // tokens: cat, <<, EOF(+body), NEWLINE, EOF
    bool found = false;
    for (const core::Token &t : toks)
      if (t.has_heredoc) {
        found = true;
        if (t.heredoc_body != "line1\nline2\n") {
          std::fprintf(stderr, "FAIL heredoc body = \"%s\"\n", t.heredoc_body.c_str());
          failures++;
        }
      }
    if (!found) {
      std::fprintf(stderr, "FAIL heredoc not collected\n");
      failures++;
    }
  }

  // Tab-stripped here-document (<<-).
  {
    auto toks = core::tokenize("cat <<-END\n\tindented\n\tEND\n");
    for (const core::Token &t : toks)
      if (t.has_heredoc && t.heredoc_body != "indented\n") {
        std::fprintf(stderr, "FAIL <<- body = \"%s\"\n", t.heredoc_body.c_str());
        failures++;
      }
  }

  return gnashtest::finish("lexer");
}
