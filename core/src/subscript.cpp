#include "gnash/core/subscript.hpp"

#include <cctype>

namespace gnash::core {

namespace {

// Advance past a single-quoted run; `i' points just after the opening quote.
// Single quotes protect everything up to the next quote (no escapes).
std::size_t skip_sq(const std::string &s, std::size_t i) {
  while (i < s.size() && s[i] != '\'') i++;
  return i < s.size() ? i + 1 : i;
}

// Advance past a `...` run; `i' points just after the opening backtick.  Like
// bash's skip_matched_pair, a backtick body is scanned only for its terminator.
std::size_t skip_bq(const std::string &s, std::size_t i) {
  while (i < s.size() && s[i] != '`') i++;
  return i < s.size() ? i + 1 : i;
}

std::size_t skip_dq(const std::string &s, std::size_t i);

// Advance past a $(...) or ${...} substitution; `i' points at the `$'.  Nested
// substitutions, quotes and escapes inside are skipped so a `)' / `}' / `]'
// buried in the substitution does not end the enclosing construct.
std::size_t skip_dollar(const std::string &s, std::size_t i) {
  char open = s[i + 1];
  char close = open == '(' ? ')' : '}';
  i += 2;
  int depth = 1;
  while (i < s.size() && depth) {
    char c = s[i];
    if (c == '\\') { i += (i + 1 < s.size()) ? 2 : 1; continue; }
    if (c == '\'') { i = skip_sq(s, i + 1); continue; }
    if (c == '"') { i = skip_dq(s, i + 1); continue; }
    if (c == '`') { i = skip_bq(s, i + 1); continue; }
    if (c == '$' && i + 1 < s.size() && (s[i + 1] == '(' || s[i + 1] == '{')) {
      i = skip_dollar(s, i);
      continue;
    }
    if (c == open) depth++;
    else if (c == close && --depth == 0) return i + 1;
    i++;
  }
  return i;
}

// Advance past a double-quoted run; `i' points just after the opening quote.
// Inside, `\' escapes the next byte and `...` / $(...) / ${...} nest.
std::size_t skip_dq(const std::string &s, std::size_t i) {
  while (i < s.size() && s[i] != '"') {
    char c = s[i];
    if (c == '\\') { i += (i + 1 < s.size()) ? 2 : 1; continue; }
    if (c == '`') { i = skip_bq(s, i + 1); continue; }
    if (c == '$' && i + 1 < s.size() && (s[i + 1] == '(' || s[i + 1] == '{')) {
      i = skip_dollar(s, i);
      continue;
    }
    i++;
  }
  return i < s.size() ? i + 1 : i;
}

}  // namespace

std::size_t skip_subscript(const std::string &s, std::size_t open) {
  std::size_t i = open + 1;
  int count = 1;  // the opening '[' already seen
  while (i < s.size()) {
    char c = s[i];
    if (c == '\\') { i += (i + 1 < s.size()) ? 2 : 1; continue; }
    if (c == '`') { i = skip_bq(s, i + 1); continue; }
    if (c == '\'') { i = skip_sq(s, i + 1); continue; }
    if (c == '"') { i = skip_dq(s, i + 1); continue; }
    if (c == '$' && i + 1 < s.size() && (s[i + 1] == '(' || s[i + 1] == '{')) {
      i = skip_dollar(s, i);
      continue;
    }
    if (c == '\n') return std::string::npos;  // a bare newline ends the word
    if (c == '[') { count++; i++; continue; }
    if (c == ']') { if (--count == 0) return i; i++; continue; }
    i++;
  }
  return std::string::npos;
}

std::size_t split_subscript(const std::string &word, std::string &name,
                            std::string &sub, bool &has_sub) {
  has_sub = false;
  std::size_t i = 0;
  if (i >= word.size() ||
      (!std::isalpha(static_cast<unsigned char>(word[i])) && word[i] != '_'))
    return std::string::npos;
  while (i < word.size() &&
         (std::isalnum(static_cast<unsigned char>(word[i])) || word[i] == '_'))
    i++;
  name = word.substr(0, i);
  if (i < word.size() && word[i] == '[') {
    std::size_t close = skip_subscript(word, i);
    if (close != std::string::npos) {
      sub = word.substr(i + 1, close - i - 1);
      has_sub = true;
      return close + 1;
    }
  }
  return i;
}

}  // namespace gnash::core
