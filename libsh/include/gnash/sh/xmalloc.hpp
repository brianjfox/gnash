// xmalloc.hpp -- checked allocation helpers shared across gnash libraries.
//
// These mirror bash's lib/malloc/xmalloc: allocation failure is fatal rather
// than something every caller must handle.  History and the other C-string
// domains use these so ownership matches the classic libraries (free() with
// xfree()).
#ifndef GNASH_SH_XMALLOC_HPP
#define GNASH_SH_XMALLOC_HPP

#include <cstddef>

namespace gnash::sh {

void *xmalloc(std::size_t n);
void *xrealloc(void *p, std::size_t n);
void  xfree(void *p);

// strdup() equivalent built on xmalloc; nullptr in -> nullptr out.
char *savestring(const char *s);

}  // namespace gnash::sh

#endif  // GNASH_SH_XMALLOC_HPP
