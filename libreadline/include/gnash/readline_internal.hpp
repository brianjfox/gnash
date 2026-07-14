// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// readline_internal.hpp -- internal shared state for the libreadline sources.
//
// Not installed as a public header; it wires state.cpp, text.cpp, and the
// coming tty/redisplay layers together.
#ifndef GNASH_READLINE_INTERNAL_HPP
#define GNASH_READLINE_INTERNAL_HPP

#include <csignal>
#include <string>

#include "readline/keymaps.h"

namespace gnash::readline {

// Set by the SIGINT handler that readline() installs while reading from a tty;
// polled by the key-read loop (input.cpp) so C-c aborts the line in progress.
extern volatile std::sig_atomic_t rl_sigint_flag;

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

// Kill-ring entry at the current ring index (what yank inserts), or nullptr
// if the ring is empty.  Each new kill resets the index to the newest entry.
const std::string *current_kill();

// Rotate the ring index to the previous (older) entry, wrapping; the entry
// rotated to, or nullptr if the ring is empty.  Used by yank-pop.
const std::string *rotate_kill();

// Discard the kill ring (used by tests).
void reset_kill_ring();

// ---- input pushback and key-recording taps (input.cpp) --------------------

// Queue KEYS so rl_read_key() returns them before reading the stream.
void stuff_input(const std::string &keys);

// A key is readable within TIMEOUT_MS (pushback queue or the input fd).
bool input_pending(int timeout_ms);

extern bool macro_recording;   // C-x ( .. C-x ): tap keys into macro_keys
extern std::string macro_keys;
extern bool redo_capturing;    // vi change command in progress: tap keys
extern std::string redo_capture;
extern std::string redo_keys;  // last completed vi change, for `.'

// ---- emacs undo (misc.cpp) -------------------------------------------------

// Discard all undo state (new line, or the line was replaced from history).
void undo_clear();

// Bumped by undo_clear(); the dispatch loop skips recording a snapshot when a
// command cleared the undo list itself (history motions must not be undoable).
unsigned undo_generation();

// Record a pre-command snapshot of the line.
void undo_push(const std::string &line, int point);

// ---- vi redo (vi_mode.cpp) -------------------------------------------------

// FN starts a vi change command (worth capturing for `.').
bool vi_change_starter(rl_command_func_t *fn);

}  // namespace gnash::readline

#endif  // GNASH_READLINE_INTERNAL_HPP
