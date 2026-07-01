// termcap_cxx.cpp -- std::string wrappers over the C termcap entry points.
#include "gnash/termcap.hpp"

#include <cstdlib>

#include "gnash/sh/xmalloc.hpp"
#include "termcap.h"

namespace gnash::termcap {

bool load(const std::string &term) {
  // tgetent copies into the provided buffer; a generous fixed size suffices for
  // our entries, but pass nullptr so the library manages storage itself.
  return tgetent(nullptr, term.c_str()) == 1;
}

std::optional<std::string> str(const char *cap) {
  char *s = tgetstr(cap, nullptr);
  if (s == nullptr) return std::nullopt;
  std::string out(s);
  gnash::sh::xfree(s);
  return out;
}

int num(const char *cap) { return tgetnum(cap); }

bool flag(const char *cap) { return tgetflag(cap) != 0; }

}  // namespace gnash::termcap
