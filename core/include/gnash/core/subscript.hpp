#ifndef GNASH_CORE_SUBSCRIPT_HPP
#define GNASH_CORE_SUBSCRIPT_HPP

#include <cstddef>
#include <string>

namespace gnash::core {

// Given s[open] == '[', return the index of the matching, unquoted ']' that
// closes the array subscript, or std::string::npos if none is found.
//
// This mirrors bash's skipsubscript()/skip_matched_pair(): a ']' does not close
// the subscript when it is backslash-escaped, inside single or double quotes,
// inside a `...` / $(...) / ${...} substitution, or nested in a further '['.
// So the key of `a[x\]y]`, `a["p]q"]`, `a[$(cmd])]` and `a[b[c]]` all scan
// correctly.  It is the single boundary primitive shared by the lexer (deciding
// an assignment word), the compound-literal element parser, the read/write of
// `name[sub]', and `unset name[sub]'.
std::size_t skip_subscript(const std::string &s, std::size_t open);

// Split a possibly-subscripted assignment target.  If `word' begins with a
// valid name optionally followed by `[subscript]', sets `name' and (when a
// subscript is present) `sub` to the text between the brackets, and returns the
// index of the first character after the closing ']' (or after the name when
// there is no subscript).  Returns std::string::npos if `word' does not start
// with a valid name.  `has_sub' reports whether a subscript was present.
std::size_t split_subscript(const std::string &word, std::string &name,
                            std::string &sub, bool &has_sub);

}  // namespace gnash::core

#endif  // GNASH_CORE_SUBSCRIPT_HPP
