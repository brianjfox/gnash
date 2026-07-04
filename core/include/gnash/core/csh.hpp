// csh.hpp -- the csh/tcsh interpreter used by gnash's csh personality.
//
// Unlike the zsh/ash/ksh personas (surface behavior over the shared Bourne
// engine), csh is a different language, so it has its own lexer, parser, and
// evaluator.  run_csh() executes a whole csh script/command and returns the
// exit status; it keeps state (variables) in the Shell so the interactive REPL
// can call it line by line.
#ifndef GNASH_CORE_CSH_HPP
#define GNASH_CORE_CSH_HPP

#include <string>

#include "gnash/core/shell.hpp"

namespace gnash::core {

int run_csh(Shell &sh, const std::string &script);

}  // namespace gnash::core

#endif  // GNASH_CORE_CSH_HPP
