// lexer_test.cpp -- shell tokenizer.

#include <cstdio>
#include <string>

#include "gnash/core/lexer.hpp"

namespace core = gnash::core;

static int failures = 0;

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

  if (failures == 0) {
    std::printf("all lexer tests passed\n");
    return 0;
  }
  std::fprintf(stderr, "%d lexer test(s) failed\n", failures);
  return 1;
}
