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
  std::string expand_no_split(const std::string &text, bool do_glob = false);

  // Expand a word that will be used as a match pattern (case patterns,
  // [[ == ]] right sides): quoted characters are backslash-escaped in the
  // result so the matcher treats them literally, while unquoted glob
  // characters stay active.
  std::string expand_pattern(const std::string &text);

  // Assignment RHS: tilde + parameter/command/arith + quote removal, no split,
  // no glob.
  std::string expand_assignment(const std::string &text);

  // Here-document body (unquoted delimiter): parameter/command/arithmetic
  // expansion and `\'-escaping of $ ` \ only.  Quote characters are literal
  // (unlike expand_no_split, which would treat them as quoting).
  std::string expand_heredoc(const std::string &text);

  // Value of a parameter (including specials); `set` reports whether it was set.
  // `defaulting_op' is set by ${x-…}/${x:-…}/${x=…}/${x+…}/${x?…} callers, where
  // an unset variable is handled by the operator and must NOT trip `set -u'.
  std::string param_value(const std::string &name, bool &set, bool defaulting_op = false);

  // Replace any <(cmd) / >(cmd) in WORD with a /dev/fd/N path, forking the inner
  // command and recording it on the shell for later cleanup.
  void extract_procsubs(std::string &word);

 private:
  Shell &sh_;

  // Core: turn one raw word into (result string, per-char quoted mask), with
  // `\x01' field-separator markers inserted for "$@" splitting.
  void process(const std::string &text, std::string &out, std::string &mask,
               bool assignment_rhs, bool heredoc = false);

  // Expand a ${...} / $name / $(...) / $((...)) starting at text[i] (i at `$').
  void expand_dollar(const std::string &text, size_t &i, bool dq, std::string &out,
                     std::string &mask);

  // zsh array subscript on NAME with the raw text SUB (between the brackets):
  // a single 1-based/negative index, or a `lo,hi' range.  Emits the selected
  // element(s) into out/mask -- a range yields one word per element (unquoted)
  // or an IFS-joined word (double-quoted).  Used for `$name[..]' and
  // `${name[lo,hi]}' under the zsh personality.
  void emit_zsh_subscript(const std::string &name, const std::string &sub, bool dq,
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

// Apply a NAME=VALUE / NAME[i]=VALUE / NAME=(...) assignment word to the shell
// (used by declare/local/readonly for array and scalar values).
void apply_assignment_word(Shell &sh, const std::string &word);

}  // namespace gnash::core

#endif  // GNASH_CORE_EXPAND_HPP
