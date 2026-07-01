// vi_mode.cpp -- vi editing mode (a functional subset of vi_mode.c).
//
// Two keymaps: insertion (like emacs typing, ESC -> command) and movement
// (motions and operators).  Mode switches flip the current keymap, which the
// dispatch loop re-reads each key.  Word motions reuse the emacs word routines;
// full vi word classes are a later refinement.

#include "gnash/readline_internal.hpp"
#include "readline/keymaps.h"
#include "readline/readline.h"

extern "C" {
int rl_editing_mode = 1;  // 1 = emacs, 0 = vi
Keymap vi_insertion_keymap = nullptr;
Keymap vi_movement_keymap = nullptr;
}

// ---- mode switches --------------------------------------------------------

extern "C" int rl_vi_editing_mode(int /*count*/, int /*key*/) {
  rl_editing_mode = 0;
  rl_set_keymap(vi_insertion_keymap);
  return 0;
}

extern "C" int rl_emacs_editing_mode(int /*count*/, int /*key*/) {
  rl_editing_mode = 1;
  rl_set_keymap(rl_get_keymap_by_name("emacs"));
  return 0;
}

// ESC in insertion mode: enter command mode, moving left one (as vi does).
static int vi_movement_mode(int /*count*/, int /*key*/) {
  rl_set_keymap(vi_movement_keymap);
  if (rl_point > 0) rl_point--;
  return 0;
}

static int vi_insert_mode(int /*count*/, int /*key*/) {
  rl_set_keymap(vi_insertion_keymap);
  return 0;
}

static int vi_append_mode(int /*count*/, int /*key*/) {
  if (rl_point < rl_end) rl_point++;
  rl_set_keymap(vi_insertion_keymap);
  return 0;
}

static int vi_insert_beg(int count, int key) {
  rl_beg_of_line(count, key);
  rl_set_keymap(vi_insertion_keymap);
  return 0;
}

static int vi_append_eol(int count, int key) {
  rl_end_of_line(count, key);
  rl_set_keymap(vi_insertion_keymap);
  return 0;
}

static int vi_delete_char(int count, int key) { return rl_delete(count, key); }

static int vi_change_to_eol(int count, int key) {
  rl_kill_line(count, key);
  rl_set_keymap(vi_insertion_keymap);
  return 0;
}

namespace {
void bind_func(Keymap m, int c, rl_command_func_t *f) {
  m[c].type = ISFUNC;
  m[c].function = f;
  m[c].kmap = nullptr;
}
}  // namespace

namespace gnash::readline {

void build_vi_keymaps() {
  if (vi_insertion_keymap) return;

  Keymap ins = rl_make_keymap();  // printable -> self-insert
  bind_func(ins, 0x1b, vi_movement_mode);  // ESC -> command mode
  bind_func(ins, '\r', rl_newline);
  bind_func(ins, '\n', rl_newline);
  bind_func(ins, 0x7f, rl_rubout);
  bind_func(ins, 0x08, rl_rubout);
  bind_func(ins, 0x04, rl_eof_or_delete);

  Keymap mov = rl_make_bare_keymap();
  bind_func(mov, 'h', rl_backward_char);
  bind_func(mov, 'l', rl_forward_char);
  bind_func(mov, ' ', rl_forward_char);
  bind_func(mov, '0', rl_beg_of_line);
  bind_func(mov, '^', rl_beg_of_line);
  bind_func(mov, '$', rl_end_of_line);
  bind_func(mov, 'w', rl_forward_word);
  bind_func(mov, 'b', rl_backward_word);
  bind_func(mov, 'e', rl_forward_word);
  bind_func(mov, 'x', vi_delete_char);
  bind_func(mov, 'D', rl_kill_line);
  bind_func(mov, 'C', vi_change_to_eol);
  bind_func(mov, 'i', vi_insert_mode);
  bind_func(mov, 'a', vi_append_mode);
  bind_func(mov, 'I', vi_insert_beg);
  bind_func(mov, 'A', vi_append_eol);
  bind_func(mov, 'k', rl_get_previous_history);
  bind_func(mov, 'j', rl_get_next_history);
  bind_func(mov, '\r', rl_newline);
  bind_func(mov, '\n', rl_newline);

  vi_insertion_keymap = ins;
  vi_movement_keymap = mov;
}

}  // namespace gnash::readline
