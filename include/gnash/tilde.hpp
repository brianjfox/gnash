// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// tilde.hpp -- modern C++ convenience wrapper over the tilde library.
//
// The full C interface (hooks, additional prefixes/suffixes) lives in
// <readline/tilde.h>; this offers std::string-returning helpers for the common
// cases.  Behaviour matches bash 5.3 lib/tilde/tilde.c.
#ifndef GNASH_TILDE_HPP
#define GNASH_TILDE_HPP

#include <string>
#include <string_view>

namespace gnash::tilde {

// Expand every tilde-prefixed word in `s` (~/foo, ~user, plus the configured
// additional prefixes).
std::string expand(std::string_view s);

// Expand a single word that begins with a tilde.
std::string expand_word(std::string_view s);

}  // namespace gnash::tilde

#endif  // GNASH_TILDE_HPP
