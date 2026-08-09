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

// A name quoted for an error message (bash printable_filename): ANSI-C form
// when it contains nonprinting/invalid bytes, otherwise the name unchanged.
std::string printable_name(const std::string &s);

// Apply one `set -o NAME' state change; false (silently) for an unknown name.
// Used by invocation flags and the env SHELLOPTS import as well as `set'.
bool apply_set_o_option(Shell &sh, const std::string &o, bool on);

// Would NAME run as a command (builtin/keyword/function/alias/PATH)?  Used by
// interactive syntax highlighting.
bool command_is_valid(Shell &sh, const std::string &name);

// Full $PATH resolution of NAME (empty when not found); a NAME containing a
// slash resolves to itself when executable.
std::string find_in_path(Shell &sh, const std::string &name);

// True for the POSIX special builtins (break, return, set, ...): posix-mode
// command lookup finds them before functions.
bool is_special_builtin_name(const std::string &n);

// Every command name beginning with PREFIX -- shell keywords, aliases,
// functions, builtins, and executables on $PATH -- sorted and de-duplicated.
// Used for command-position tab completion.
std::vector<std::string> command_completions(Shell &sh, const std::string &prefix);

// Populate sh.shopt_opts with the default shopt option states (idempotent).
// Called at startup so $BASHOPTS reflects the defaults before any `shopt' runs.
void shopt_seed(Shell &sh);

// The `declare -p'-form string that recreates variable V named NAME (e.g.
// "declare -A a=([k]=\"v\" )"), without a trailing newline.  CMD selects the
// builtin personality for POSIX readonly/export output.  Shared with the
// ${var@A} parameter transformation so both render identically.
std::string declare_var_string(const std::string &name, const Variable &v,
                               const std::string &cmd = "declare",
                               bool posix = false);

// Quote WORD as bash's `set -x' (xtrace) does: '' for empty, $'...' for a word
// with non-printable bytes, '...' for one with shell metacharacters, else bare.
std::string xtrace_quote_word(const std::string &w);

}  // namespace gnash::core

#endif  // GNASH_CORE_BUILTINS_HPP
