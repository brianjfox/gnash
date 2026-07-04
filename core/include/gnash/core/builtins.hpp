// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// builtins.hpp -- shell builtin commands.
#ifndef GNASH_CORE_BUILTINS_HPP
#define GNASH_CORE_BUILTINS_HPP

#include <string>
#include <vector>

#include "gnash/core/shell.hpp"

namespace gnash::core {

// If argv[0] names a builtin, run it, store the exit status in *status, and
// return true.  Otherwise return false.
bool run_builtin(Shell &sh, const std::vector<std::string> &argv, int *status);

// Would NAME run as a command (builtin/keyword/function/alias/PATH)?  Used by
// interactive syntax highlighting.
bool command_is_valid(Shell &sh, const std::string &name);

}  // namespace gnash::core

#endif  // GNASH_CORE_BUILTINS_HPP
