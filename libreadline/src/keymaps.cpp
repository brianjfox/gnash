// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// keymaps.cpp -- keymap allocation and the emacs key bindings.
//
// The emacs keymaps are built programmatically rather than as large static
// arrays (as in emacs_keymap.c): the same bindings, less transcription risk.

#include <cstring>

#include "gnash/readline_internal.hpp"
#include "gnash/sh/xmalloc.hpp"
#include "readline/keymaps.h"
#include "readline/readline.h"

using gnash::sh::xfree;
using gnash::sh::xmalloc;

// ---- exported keymaps -----------------------------------------------------
extern "C" {
Keymap emacs_standard_keymap = nullptr;
Keymap emacs_meta_keymap = nullptr;
Keymap emacs_ctlx_keymap = nullptr;
}

// The keymap for CSI/SS3 sequences (arrow and navigation keys): ESC [ x and
// ESC O x.  Not part of the classic public set, so file-local.
static Keymap esc_bracket_keymap = nullptr;
static Keymap current_keymap = nullptr;

extern "C" Keymap rl_make_bare_keymap(void) {
  Keymap km = static_cast<Keymap>(xmalloc(KEYMAP_SIZE * sizeof(KEYMAP_ENTRY)));
  for (int i = 0; i < KEYMAP_SIZE; i++) {
    km[i].type = ISFUNC;
    km[i].function = nullptr;
    km[i].kmap = nullptr;
  }
  return km;
}

extern "C" Keymap rl_copy_keymap(Keymap map) {
  Keymap km = rl_make_bare_keymap();
  for (int i = 0; i < KEYMAP_SIZE; i++) km[i] = map[i];
  return km;
}

extern "C" void rl_discard_keymap(Keymap map) { xfree(map); }

extern "C" Keymap rl_get_keymap(void) { return current_keymap; }
extern "C" void rl_set_keymap(Keymap map) { current_keymap = map; }

extern "C" Keymap rl_get_keymap_by_name(const char *name) {
  if (name == nullptr) return nullptr;
  if (std::strcmp(name, "emacs") == 0 || std::strcmp(name, "emacs-standard") == 0)
    return emacs_standard_keymap;
  if (std::strcmp(name, "emacs-meta") == 0) return emacs_meta_keymap;
  if (std::strcmp(name, "emacs-ctlx") == 0) return emacs_ctlx_keymap;
  return nullptr;
}

extern "C" Keymap rl_make_keymap(void) {
  Keymap km = rl_make_bare_keymap();
  for (int i = ' '; i < 127; i++) {
    km[i].type = ISFUNC;
    km[i].function = rl_insert;
  }
  return km;
}

namespace {

void bind_func(Keymap m, int c, rl_command_func_t *f) {
  m[c].type = ISFUNC;
  m[c].function = f;
  m[c].kmap = nullptr;
}

void bind_map(Keymap m, int c, Keymap sub) {
  m[c].type = ISKMAP;
  m[c].function = nullptr;
  m[c].kmap = sub;
}

}  // namespace

namespace gnash::readline {

void build_emacs_keymaps() {
  if (emacs_standard_keymap) return;  // already built

  Keymap std_km = rl_make_keymap();   // printable -> self-insert
  Keymap meta = rl_make_bare_keymap();
  Keymap ctlx = rl_make_bare_keymap();
  Keymap esc = rl_make_bare_keymap();

  // Control keys in the standard keymap.
  bind_func(std_km, 0x01, rl_beg_of_line);        // C-a
  bind_func(std_km, 0x02, rl_backward_char);      // C-b
  bind_func(std_km, 0x04, rl_eof_or_delete);      // C-d
  bind_func(std_km, 0x05, rl_end_of_line);        // C-e
  bind_func(std_km, 0x06, rl_forward_char);       // C-f
  bind_func(std_km, 0x08, rl_rubout);             // C-h (backspace)
  bind_func(std_km, 0x0a, rl_newline);            // C-j (LF) -> accept
  bind_func(std_km, 0x0b, rl_kill_line);          // C-k
  bind_func(std_km, 0x0c, rl_clear_screen);       // C-l -> clear screen
  bind_func(std_km, 0x0d, rl_newline);            // C-m (CR) -> accept
  bind_func(std_km, 0x0e, rl_get_next_history);   // C-n
  bind_func(std_km, 0x10, rl_get_previous_history);  // C-p
  bind_func(std_km, 0x14, rl_transpose_chars);    // C-t
  bind_func(std_km, 0x15, rl_unix_line_discard);  // C-u
  bind_func(std_km, 0x17, rl_unix_word_rubout);   // C-w
  bind_func(std_km, 0x19, rl_yank);               // C-y
  bind_func(std_km, 0x7f, rl_rubout);             // DEL
  bind_func(std_km, 0x09, rl_complete);           // TAB -> complete
  bind_func(std_km, 0x12, rl_reverse_search_history);  // C-r
  bind_func(std_km, 0x13, rl_forward_search_history);  // C-s
  bind_map(std_km, 0x1b, meta);                   // ESC -> meta
  bind_map(std_km, 0x18, ctlx);                   // C-x -> ctlx prefix

  // Meta (ESC-prefixed) bindings.
  bind_func(meta, 'b', rl_backward_word);
  bind_func(meta, 'f', rl_forward_word);
  bind_func(meta, 'd', rl_kill_word);
  bind_func(meta, 0x7f, rl_backward_kill_word);
  bind_func(meta, 0x08, rl_backward_kill_word);
  bind_func(meta, 'u', rl_upcase_word);
  bind_func(meta, 'l', rl_downcase_word);
  bind_func(meta, 'c', rl_capitalize_word);
  for (int d = '0'; d <= '9'; d++) bind_func(meta, d, rl_digit_argument);
  bind_func(meta, '-', rl_digit_argument);
  bind_func(meta, '?', rl_possible_completions);
  bind_func(meta, '.', rl_yank_last_arg);
  bind_func(meta, '_', rl_yank_last_arg);
  bind_func(meta, 0x19, rl_yank_nth_arg);         // M-C-y
  bind_map(meta, '[', esc);   // ESC [ ... (CSI)
  bind_map(meta, 'O', esc);   // ESC O ... (SS3, application cursor keys)

  // CSI/SS3 final bytes: the arrow and navigation keys.
  bind_func(esc, 'A', rl_get_previous_history);  // up
  bind_func(esc, 'B', rl_get_next_history);       // down
  bind_func(esc, 'C', rl_forward_char);           // right
  bind_func(esc, 'D', rl_backward_char);          // left
  bind_func(esc, 'H', rl_beg_of_line);            // home
  bind_func(esc, 'F', rl_end_of_line);            // end

  emacs_standard_keymap = std_km;
  emacs_meta_keymap = meta;
  emacs_ctlx_keymap = ctlx;
  esc_bracket_keymap = esc;
  current_keymap = std_km;
}

}  // namespace gnash::readline
