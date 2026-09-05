// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// quote.hpp -- word-quoting idioms shared by the builtins and the expander.
//
// A handful of ways of re-quoting a string for later re-parsing by the shell
// recur through the code: the `alias'/'declare -p'/'set' listings, xtrace
// word rendering, the ${var@Q}/@K parameter transformations, compspec
// display.  These helpers are the single home for those idioms so the rules
// (and any future fix to them) live in one place:
//   - single_quote:         '...' with an embedded ' spelled '\''
//                           (bash sh_single_quote)
//   - double_quote:         "..." escaping " \ $ `
//                           (bash sh_double_quote)
//   - contains_shell_metas: a word holds a shell metacharacter forcing
//                           quoting (bash sh_contains_shell_metas)
//   - has_nonprint:         a byte string holds a non-printable byte
//                           (bash ansic_shouldquote)
//   - ansic_quote:          $'...' with common escapes spelled out and the
//                           other non-printables as \NNN octal
//
// They are byte-oriented on purpose: the locale/UTF-8-aware pair of
// non-printable test and ANSI-C quoting is q_needs_ansic/q_ansic in
// builtins.cpp, which the `declare -p' and xtrace paths keep using.

#ifndef GNASH_CORE_QUOTE_HPP
#define GNASH_CORE_QUOTE_HPP

#include <cstdio>
#include <string>

namespace gnash::core {

// Wrap S in single quotes; an embedded single quote becomes '\''.  Always
// quotes, even a word that needs no quoting; an empty S renders as ''.
inline std::string single_quote(const std::string &s) {
  std::string r = "'";
  for (char c : s) {
    if (c == '\'') r += "'\\''";
    else r += c;
  }
  return r + "'";
}

// Wrap S in double quotes, backslash-escaping the characters special inside
// "..." (" \ $ `) -- the form `declare -p' and `set' use for plain values.
inline std::string double_quote(const std::string &s) {
  std::string r = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\' || c == '$' || c == '`') r += '\\';
    r += c;
  }
  return r + "\"";
}

// True if S holds a shell metacharacter that forces quoting when the string
// is re-read (bash's sh_contains_shell_metas): whitespace and the quote,
// backslash, operator and glob characters anywhere, plus `~' (also right
// after `=' or `:') and `#' at the start of the string.
inline bool contains_shell_metas(const std::string &s) {
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    switch (c) {
      case ' ': case '\t': case '\n': case '\'': case '"': case '\\':
      case '|': case '&': case ';': case '(': case ')': case '<': case '>':
      case '!': case '{': case '}': case '*': case '[': case '?': case ']':
      case '^': case '$': case '`':
        return true;
      case '~':
        if (i == 0 || s[i - 1] == '=' || s[i - 1] == ':') return true;
        break;
      case '#':
        if (i == 0) return true;
        break;
    }
  }
  return false;
}

// True if S contains a non-printable byte (bash's ansic_shouldquote,
// byte-oriented): any control byte or DEL.
inline bool has_nonprint(const std::string &s) {
  for (unsigned char c : s)
    if (c < 32 || c == 127) return true;
  return false;
}

// ANSI-C ($'...') quoting of S: the common escapes spelled \n \t \r \\ \' ,
// every other non-printable byte as \NNN octal, printable bytes verbatim.
inline std::string ansic_quote(const std::string &s) {
  std::string r = "$'";
  for (unsigned char c : s) {
    switch (c) {
      case '\n': r += "\\n"; break;
      case '\t': r += "\\t"; break;
      case '\r': r += "\\r"; break;
      case '\\': r += "\\\\"; break;
      case '\'': r += "\\'"; break;
      default:
        if (c < 32 || c == 127) {
          char b[8];
          std::snprintf(b, sizeof b, "\\%03o", c);
          r += b;
        } else r += static_cast<char>(c);
    }
  }
  return r + "'";
}

}  // namespace gnash::core

#endif  // GNASH_CORE_QUOTE_HPP
