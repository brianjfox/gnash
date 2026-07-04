// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// glob.hpp -- modern C++ surface for the glob library.
//
// Pattern matching (fnmatch) and filename globbing, faithful to bash 5.3's
// lib/glob.  The full C interface is in <strmatch.h> and <glob.h>.
#ifndef GNASH_GLOB_HPP
#define GNASH_GLOB_HPP

#include <string>
#include <string_view>
#include <vector>

namespace gnash::glob {

// Match `str` against shell pattern `pat`.  `flags` are the FNM_* bits from
// <strmatch.h> (default enables extended globbing).
bool fnmatch(std::string_view pat, std::string_view str, int flags = (1 << 5));

// Expand a pathname pattern to the sorted list of matching files.  `flags` are
// the GX_* bits from <glob.h>.  Returns an empty vector if nothing matches.
std::vector<std::string> glob(std::string_view pattern, int flags = 0);

}  // namespace gnash::glob

#endif  // GNASH_GLOB_HPP
