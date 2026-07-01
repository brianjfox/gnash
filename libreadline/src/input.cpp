// input.cpp -- reading keys from rl_instream.
#include <cstdio>

#include "readline/readline.h"

extern "C" int rl_read_key(void) {
  if (rl_instream == nullptr) rl_instream = stdin;
  int c = std::getc(rl_instream);  // returns 0..255 or EOF (-1)
  return c;
}
