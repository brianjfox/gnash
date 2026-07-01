// readline_cxx.cpp -- std::string facade over the editing state.
#include "gnash/readline.hpp"

#include <string>

#include "gnash/readline_internal.hpp"
#include "readline/readline.h"

namespace gnash::readline {

void set_line(std::string_view s) {
  std::string tmp(s);
  rl_replace_line(tmp.c_str(), 1);
  rl_point = rl_end;
}

std::string line() {
  maybe_init_line();
  return std::string(rl_line_buffer, static_cast<size_t>(rl_end));
}

int point() { return rl_point; }

void set_point(int p) {
  if (p < 0) p = 0;
  if (p > rl_end) p = rl_end;
  rl_point = p;
}

int end() { return rl_end; }

}  // namespace gnash::readline
