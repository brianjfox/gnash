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

struct ParseResult {
  CommandPtr command;   // null for empty input
  bool ok = true;
  std::string error;    // set when ok == false (may be multiple lines)
  int error_line = 0;   // 1-based source line of the failure
  bool incomplete = false;  // input ended mid-construct (needs more lines)
  // A here-document body was delimited by end of input: ok stays true and the
  // command is runnable (bash runs it with a warning), but incomplete is also
  // set so line-at-a-time readers keep accumulating input.
  bool heredoc_eof = false;
  std::string heredoc_eof_delim;
  int heredoc_eof_line = 0;
};

// Parse a complete program.
ParseResult parse(const std::string &input);

// Parse with alias expansion applied first: regular aliases in command position,
// zsh global aliases (`alias -g') anywhere, and zsh suffix aliases (`alias -s').
// The global/suffix maps are empty outside the zsh personality.
ParseResult parse_with_aliases(const std::string &input,
                               const std::map<std::string, std::string> &aliases,
                               const std::map<std::string, std::string> &global_aliases = {},
                               const std::map<std::string, std::string> &suffix_aliases = {});

}  // namespace gnash::core

#endif  // GNASH_CORE_PARSER_HPP
