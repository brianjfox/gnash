// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// gnash_rl_demo -- a minimal interactive REPL exercising libreadline.
//
//   $ ./build/libreadline/gnash_rl_demo
//   gnash> type here, edit with emacs keys, Up/Down for history
//
// Not a test (it needs a real terminal); build target for manual verification.

#include <cstdio>
#include <cstdlib>

#include "readline/history.h"
#include "readline/readline.h"

int main() {
  std::printf("gnash readline demo -- ^D on an empty line to quit.\n");
  char *line;
  while ((line = readline("gnash> ")) != nullptr) {
    if (*line) add_history(line);
    std::printf("=> %s\n", line);
    std::free(line);
  }
  std::printf("bye\n");
  return 0;
}
