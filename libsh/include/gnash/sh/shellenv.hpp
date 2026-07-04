// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// shellenv.hpp -- the shell-environment interface that Readline/Tilde use to
// resolve variables and the home directory.
//
// bash satisfies these symbols from the shell itself so Readline sees shell
// variables, not just the process environment.  gnash keeps that seam: libsh
// provides getenv/getpwuid-based defaults, and the shell core can install its
// own variable lookup with set_env_provider() so `~` and prompt expansion see
// shell state.
#ifndef GNASH_SH_SHELLENV_HPP
#define GNASH_SH_SHELLENV_HPP

namespace gnash::sh {

using EnvProvider = char *(*)(const char *);

// Install the function backing sh_get_env_value(); nullptr restores getenv.
void set_env_provider(EnvProvider p);

}  // namespace gnash::sh

extern "C" {
// Value of a variable, or NULL.  The returned pointer is borrowed (do not free).
char *sh_get_env_value(const char *name);
// The current user's home directory (cached; borrowed).
char *sh_get_home_dir(void);
}

#endif  // GNASH_SH_SHELLENV_HPP
