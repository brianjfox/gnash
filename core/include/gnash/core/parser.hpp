// parser.hpp -- recursive-descent parser producing the command AST.
//
// The grammar mirrors bash 5.3's parse.y productions (lists, and-or, pipelines,
// simple and compound commands, redirections, function definitions).  Not yet
// covered: [[ ]], (( )), select, coproc, arithmetic-for -- these currently
// parse as simple commands.
#ifndef GNASH_CORE_PARSER_HPP
#define GNASH_CORE_PARSER_HPP

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

}  // namespace gnash::core

#endif  // GNASH_CORE_PARSER_HPP
