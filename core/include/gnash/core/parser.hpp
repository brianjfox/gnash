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
  std::string error;    // set when ok == false
  bool incomplete = false;  // input ended mid-construct (needs more lines)
};

// Parse a complete program.
ParseResult parse(const std::string &input);

// Parse with alias expansion applied to command-position words first.
ParseResult parse_with_aliases(const std::string &input,
                               const std::map<std::string, std::string> &aliases);

}  // namespace gnash::core

#endif  // GNASH_CORE_PARSER_HPP
