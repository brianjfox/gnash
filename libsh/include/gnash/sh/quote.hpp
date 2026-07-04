// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// quote.hpp -- shell-style quoting helpers (subset of bash lib/sh/shquote.c).
#ifndef GNASH_SH_QUOTE_HPP
#define GNASH_SH_QUOTE_HPP

namespace gnash::sh {

// Return a single-quoted copy of `string` (xmalloc'd; caller frees), with any
// embedded single quotes rendered as the classic '\'' sequence.
char *single_quote(const char *string);

}  // namespace gnash::sh

#endif  // GNASH_SH_QUOTE_HPP
