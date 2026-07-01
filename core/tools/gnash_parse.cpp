// gnash-parse -- syntax-check or dump the AST of a shell script.
//
//   gnash-parse [file]      # reads stdin if no file
//   gnash-parse -n [file]   # syntax check only (like `bash -n`), no dump
//
// Exit status: 0 accepted, 2 syntax error -- matching `bash -n`.

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "gnash/core/parser.hpp"

int main(int argc, char **argv) {
  bool check_only = false;
  const char *path = nullptr;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-n")
      check_only = true;
    else
      path = argv[i];
  }

  std::string input;
  if (path) {
    std::ifstream f(path);
    if (!f) {
      std::fprintf(stderr, "gnash-parse: cannot open %s\n", path);
      return 2;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    input = ss.str();
  } else {
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    input = ss.str();
  }

  gnash::core::ParseResult r = gnash::core::parse(input);
  if (!r.ok) {
    std::fprintf(stderr, "gnash-parse: syntax error: %s\n", r.error.c_str());
    return 2;
  }
  if (!check_only)
    std::printf("%s\n", gnash::core::to_string(r.command.get()).c_str());
  return 0;
}
