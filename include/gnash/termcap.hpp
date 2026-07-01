// termcap.hpp -- modern C++ convenience wrapper over the termcap library.
//
// The full C interface is in <termcap.h>.  These helpers load an entry and
// query capabilities without manual buffer management.  A loaded entry is
// process-global (as in classic termcap), so this is not thread-safe.
#ifndef GNASH_TERMCAP_HPP
#define GNASH_TERMCAP_HPP

#include <optional>
#include <string>

namespace gnash::termcap {

// Load the entry for `term` (e.g. from $TERM).  Returns true if found.
bool load(const std::string &term);

// String capability (e.g. "cl", "ce", "cm"), with ^/\ escapes decoded.
std::optional<std::string> str(const char *cap);

// Numeric capability (e.g. "co", "li"); -1 if absent.
int num(const char *cap);

// Boolean capability (e.g. "am"); true if present.
bool flag(const char *cap);

}  // namespace gnash::termcap

#endif  // GNASH_TERMCAP_HPP
