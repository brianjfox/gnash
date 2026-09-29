// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// readline.hpp -- modern C++ surface for gnash's Readline reimplementation.
//
// In progress.  Today this exposes the editing state so callers and tests can
// drive the editing core; the interactive readline() entry point, keymap
// dispatch, completion, and redisplay are being layered in.  The full classic
// C API lives in <readline/readline.h>.
#ifndef GNASH_READLINE_HPP
#define GNASH_READLINE_HPP

#include <string>
#include <string_view>

namespace gnash::readline {

// Load `s` into the line buffer with the cursor at the end.
void set_line(std::string_view s);

// The current line contents.
std::string line();

// Cursor position within the line.
int point();
void set_point(int p);

// The line length.
int end();

// True while readline() has the terminal in its own (cbreak, no-echo) mode,
// i.e. between prep_terminal and deprep_terminal.  The shell consults this
// before recording the terminal's settings as its saved state (bash checks
// RL_STATE_TERMPREPPED for the same reason): a command run from inside
// readline -- a programmable-completion function, say -- must not make
// readline's transient modes the state later restored after a signal.
bool terminal_prepped();

}  // namespace gnash::readline

#endif  // GNASH_READLINE_HPP
