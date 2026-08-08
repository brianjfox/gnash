// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// parser.hpp -- recursive-descent parser producing the command AST.
//
// The grammar mirrors bash 5.3's parse.y productions (lists, and-or, pipelines,
// simple and compound commands, redirections, function definitions).  Not yet
// covered: [[ ]], (( )), select, coproc, arithmetic-for -- these currently
// parse as simple commands.
#ifndef GNASH_CORE_PARSER_HPP
#define GNASH_CORE_PARSER_HPP

#include <map>
#include <string>

#include "gnash/core/ast.hpp"

namespace gnash::core {

// The `[[ ... ]]' body is reconstructed into a string that the conditional
// evaluator re-tokenizes.  Characters the lexer would treat as operators or
// separators cannot survive that trip literally, so inside a `=~' operand they
// are replaced by COND_RX_ESC plus the matching letter from kCondRxSub, and the
// evaluator puts them back before expanding the operand.  A backslash would not
// do: the expander records backslash-escaped characters as QUOTED, and quoting
// is exactly what decides whether a regex metacharacter matches literally.
constexpr char COND_RX_ESC = '\x1d';        // GS -- an ordinary character to the lexer
inline constexpr const char *kCondRxRaw = "()|&;<> \t";
inline constexpr const char *kCondRxSub = "abcdefghi";

struct ParseResult {
  CommandPtr command;   // null for empty input
  bool ok = true;
  std::string error;    // set when ok == false (may be multiple lines)
  int error_line = 0;   // 1-based source line of the failure
  // The last source line this parse consumed.  bash leaves `line_number' here
  // when it hands a command to the executor, and compound commands that do not
  // install a line of their own (`while', `if', `case', `{ }', a function
  // definition) report diagnostics against it -- so a redirection error on a
  // multi-line `while ... done > f' names the `done' line, not the `while'.
  int end_line = 0;
  bool incomplete = false;  // input ended mid-construct (needs more lines)
  bool assign_error = false;  // a compound-assignment syntax error (`a=(x & y)'): $?=1, not 2
  // A here-document body was delimited by end of input: ok stays true and the
  // command is runnable (bash runs it with a warning), but incomplete is also
  // set so line-at-a-time readers keep accumulating input.
  bool heredoc_eof = false;
  std::string heredoc_eof_delim;
  int heredoc_eof_line = 0;
  bool heredoc_eof_quoted = false;
  // Here-documents left pending when a $(...) closed on the same line (bash
  // warns `command substitution: N unterminated here-document').
  int comsub_unterm = 0;
  int comsub_unterm_line = 0;
};

// Parse a complete program.
// CONT_LINES: see tokenize() in lexer.hpp -- the lines whose `\'-newline
// continuation was spliced away, so token lines can add them back.
ParseResult parse(const std::string &input, bool posix_mode = false,
                  const std::vector<int> *cont_lines = nullptr);

// Parse with alias expansion applied first: regular aliases in command position,
// zsh global aliases (`alias -g') anywhere, and zsh suffix aliases (`alias -s').
// The global/suffix maps are empty outside the zsh personality.
ParseResult parse_with_aliases(const std::string &input,
                               const std::map<std::string, std::string> &aliases,
                               const std::map<std::string, std::string> &global_aliases = {},
                               const std::map<std::string, std::string> &suffix_aliases = {},
                               bool posix_mode = false,
                               const std::vector<int> *cont_lines = nullptr);

}  // namespace gnash::core

#endif  // GNASH_CORE_PARSER_HPP
