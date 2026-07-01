#include "gnash/sh/quote.hpp"

#include <cstring>

#include "gnash/sh/xmalloc.hpp"

namespace gnash::sh {

char *single_quote(const char *string) {
  const char *s = string ? string : "";
  char *result = static_cast<char *>(xmalloc(3 + 4 * std::strlen(s)));
  char *r = result;
  *r++ = '\'';
  for (int c; (c = static_cast<unsigned char>(*s)) != 0; s++) {
    *r++ = static_cast<char>(c);
    if (c == '\'') {
      // `'\''` -- close, escaped quote, reopen.
      *r++ = '\\';
      *r++ = '\'';
      *r++ = '\'';
    }
  }
  *r++ = '\'';
  *r = '\0';
  return result;
}

}  // namespace gnash::sh
