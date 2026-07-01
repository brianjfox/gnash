// readline_internal.hpp -- internal shared state for the libreadline sources.
//
// Not installed as a public header; it wires state.cpp, text.cpp, and the
// coming tty/redisplay layers together.
#ifndef GNASH_READLINE_INTERNAL_HPP
#define GNASH_READLINE_INTERNAL_HPP

#include <string>

#include "readline/keymaps.h"

namespace gnash::readline {

// Build (once) the emacs keymaps and select the standard keymap as current.
void build_emacs_keymaps();

// Build (once) the vi insertion/movement keymaps.
void build_vi_keymaps();

// Terminal raw-mode setup / restore (no-ops on non-ttys); see tty.cpp.
void prep_terminal(int fd);
void deprep_terminal(int fd);

// Ensure rl_line_buffer is allocated.
void maybe_init_line();

// Set nonzero while a kill command is in progress so consecutive kills
// accumulate into the newest kill-ring entry.
extern int last_command_was_kill;

// Kill [from, to) into the ring (dir > 0 forward/append, dir < 0 backward/
// prepend) and remove it from the line.
void kill_text(int from, int to, int dir);

// Newest kill-ring entry (what yank inserts), or nullptr if empty.
const std::string *current_kill();

// Discard the kill ring (used by tests).
void reset_kill_ring();

}  // namespace gnash::readline

#endif  // GNASH_READLINE_INTERNAL_HPP
