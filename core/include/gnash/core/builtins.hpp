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

}  // namespace gnash::core

#endif  // GNASH_CORE_BUILTINS_HPP
