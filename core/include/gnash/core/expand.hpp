// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// expand.hpp -- word expansion.
//
// Implements the bash expansion pipeline in order (subst.c): brace expansion,
// tilde, parameter/${...}, command substitution, arithmetic, word splitting on
// IFS, pathname (glob), and quote removal.
#ifndef GNASH_CORE_EXPAND_HPP
#define GNASH_CORE_EXPAND_HPP

#include <string>
#include <vector>

#include "gnash/core/ast.hpp"
#include "gnash/core/shell.hpp"

namespace gnash::core {

class Expander {
 public:
  explicit Expander(Shell &sh) : sh_(sh) {}

  // Full pipeline for command arguments: brace -> ... -> split -> glob.
  std::vector<std::string> expand_args(const std::vector<Word> &words);

  // Expand a single word without word-splitting (redirect targets, here-doc
  // bodies with `do_expand`, case subjects).  `do_glob` controls pathname
  // expansion (used for redirect filenames).
  std::string expand_no_split(const std::string &text, bool do_glob = false,
                              bool do_procsub = true);

  // Expand a word that will be used as a match pattern (case patterns,
  // [[ == ]] right sides): quoted characters are backslash-escaped in the
  // result so the matcher treats them literally, while unquoted glob
  // characters stay active.
  std::string expand_pattern(const std::string &text);

  // Expand a word used as a REGEX (a `[[ =~ ]]' right-hand side).  Like
  // expand_pattern, but only those quoted characters that are special in a
  // POSIX ERE are backslash-escaped: `\a' is not a portable way to write a
  // literal `a', so escaping everything would change what the pattern means.
  std::string expand_regex(const std::string &text);

  // Expand a word as if inside double quotes (bash-family default words after
  // ${x:-w}/${x:+w}): `$'/backquote expand, backslash escapes the shell
  // specials, single quotes are literal.  Not used under the zsh personality,
  // whose quoting rules differ.
  std::string expand_dq_word(const std::string &w);
  // Arithmetic-context expansion: like a double-quoted string, but unescaped
  // double quotes are removed and single quotes stay ordinary characters.
  std::string expand_arith(const std::string &text);

  // Assignment RHS: tilde + parameter/command/arith + quote removal, no split,
  // no glob.
  std::string expand_assignment(const std::string &text);

  // Expand the substitute word of ${x-word} / ${x+word}.  In a splitting,
  // unquoted, bash-family context a word with an unquoted $*/$@/[*]/[@] keeps
  // its field boundaries: it stashes the expanded (out, mask) in op_out_/op_mask_
  // (with op_fields_ set) for the ${...} caller to splice, and returns "".
  // Otherwise it returns the flat expand_dq_word / expand_no_split result.
  std::string expand_op_word(const std::string &w, bool dq, bool top_level);

  // Here-document body (unquoted delimiter): parameter/command/arithmetic
  // expansion and `\'-escaping of $ ` \ only.  Quote characters are literal
  // (unlike expand_no_split, which would treat them as quoting).
  std::string expand_heredoc(const std::string &text);

  // Value of a parameter (including specials); `set` reports whether it was set.
  // `defaulting_op' is set by ${x-…}/${x:-…}/${x=…}/${x+…}/${x?…} callers, where
  // an unset variable is handled by the operator and must NOT trip `set -u'.
  std::string param_value(const std::string &name, bool &set, bool defaulting_op = false,
                          bool braced = false);

  // Replace any <(cmd) / >(cmd) in WORD with a /dev/fd/N path, forking the inner
  // command and recording it on the shell for later cleanup.
  void extract_procsubs(std::string &word);

 private:
  Shell &sh_;

  // True only while expanding a word whose result WILL be field-split (the
  // expand_args path).  In that context an unquoted $* / ${a[*]} yields separate
  // fields (like $@) via hard FIELD_SEP markers; everywhere else (assignment
  // RHS, expand_no_split, patterns, here-docs -- results that get flattened or
  // IFS[0]-joined) it joins with the first IFS character.  Toggled with save /
  // restore around each entry point's process() call.
  bool splitting_ = false;

  // A ${x-word}/${x+word} substitute word that must keep its field structure
  // stashes its expanded (out, mask) here for expand_dollar to splice, rather
  // than flattening to a string (which would lose masks and mis-handle data
  // bytes that happen to equal FIELD_SEP/QNULL).  op_fields_ signals a pending
  // splice; expand_dollar clears it before, and consumes it after, each body.
  bool op_fields_ = false;
  std::string op_out_, op_mask_;

  // Expand W (a substitute word) into op_out_/op_mask_ preserving field markers,
  // and set op_fields_.  Called only for an unquoted splat in a splitting context.
  void expand_word_fields(const std::string &w);

  // Core: turn one raw word into (result string, per-char quoted mask), with
  // `\x01' field-separator markers inserted for "$@" splitting.
  void process(const std::string &text, std::string &out, std::string &mask,
               bool assignment_rhs, bool heredoc = false, bool sq_literal = false);
  // Process double-quoted content (no surrounding quotes) from text[i] into
  // out/mask, stopping at an unescaped closing quote or end of string.
  void process_dq(const std::string &text, size_t &i, std::string &out, std::string &mask);

  // Expand a ${...} / $name / $(...) / $((...)) starting at text[i] (i at `$').
  void expand_dollar(const std::string &text, size_t &i, bool dq, std::string &out,
                     std::string &mask, bool heredoc = false);

  // zsh array subscript on NAME with the raw text SUB (between the brackets):
  // a single 1-based/negative index, or a `lo,hi' range.  Emits the selected
  // element(s) into out/mask -- a range yields one word per element (unquoted)
  // or an IFS-joined word (double-quoted).  Used for `$name[..]' and
  // `${name[lo,hi]}' under the zsh personality.
  void emit_zsh_subscript(const std::string &name, const std::string &sub, bool dq,
                          std::string &out, std::string &mask);

  // Emit a list of array elements with `${a[@]}'/`${a[*]}' field/quote
  // semantics (SEL is '@' or '*'), honoring the current quoting/splitting.
  // Shared by the ${a[@]} path and namerefs/indirection to a whole-array splat.
  void emit_array_items(const std::vector<std::string> &items, char sel, bool dq,
                        std::string &out, std::string &mask);

  // zsh `${(flags)name}' expansion flags (join/split/sort/unique/case/keys).
  // Returns true and emits into out/mask when BODY begins with a `(flags)'
  // group; false (leaving out/mask untouched) otherwise, so the caller can
  // fall through to ordinary ${...} handling.
  bool emit_zsh_flags(const std::string &body, bool dq, std::string &out, std::string &mask);

  // Split into fields on IFS; each field carries its per-character quote mask
  // so pathname expansion can tell quoted metacharacters from unquoted ones.
  std::vector<std::pair<std::string, std::string>> split_ifs(const std::string &s,
                                                             const std::string &mask);
  std::vector<std::string> glob_field(const std::string &field, const std::string &mask);
};

// Brace expansion on a single word (textual, pre-expansion).
std::vector<std::string> brace_expand(const std::string &text);

// Character-aware whole-string case folding (for `declare -u/-l/-c'): upper,
// lower, or capitalize-first-lowercase-rest, honoring the multibyte locale.
std::string mb_upper(const std::string &s);
std::string mb_lower(const std::string &s);
std::string mb_capitalize(const std::string &s);

// Apply a NAME=VALUE / NAME[i]=VALUE / NAME=(...) assignment word to the shell
// (used by declare/local/readonly for array and scalar values).
// Apply a NAME=... / NAME=(...) word.  CTX names whoever is answerable for a
// failure: a builtin's own name, "" for no attribution, or null to let the
// enclosing function answer -- which is what a compound assignment WORD does,
// since bash performs it before the builtin is entered.
void apply_assignment_word(Shell &sh, const std::string &word, const char *ctx = nullptr);

// Encode a Unicode code point for \u/\U (bash u32cconv): the locale charset's
// bytes when LC_CTYPE is not UTF-8 (via iconv), UTF-8 otherwise; a code point
// iconv cannot represent appends the ISO C99 escape text (\uXXXX/\UXXXXXXXX)
// and one >= 0x80000000 appends nothing.  Used by $'...', printf, echo -e.
void append_utf8(std::string &out, unsigned long cp);

}  // namespace gnash::core

#endif  // GNASH_CORE_EXPAND_HPP
