#include "gnash/sh/xmalloc.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gnash::sh {

namespace {
void *check(void *p, std::size_t n) {
  if (p == nullptr && n != 0) {
    std::fprintf(stderr, "gnash: out of virtual memory\n");
    std::abort();
  }
  return p;
}
}  // namespace

void *xmalloc(std::size_t n) { return check(std::malloc(n), n); }

void *xrealloc(void *p, std::size_t n) {
  return p ? check(std::realloc(p, n), n) : check(std::malloc(n), n);
}

void xfree(void *p) {
  if (p) std::free(p);
}

char *savestring(const char *s) {
  if (s == nullptr) return nullptr;
  std::size_t n = std::strlen(s) + 1;
  char *r = static_cast<char *>(xmalloc(n));
  std::memcpy(r, s, n);
  return r;
}

}  // namespace gnash::sh
