// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// vi_mode.cpp -- vi editing mode (a usable subset of vi_mode.c).
//
// Two keymaps: insertion (like emacs typing, ESC -> command) and movement
// (motions, operators, edits).  Mode switches flip the current keymap, which
// the dispatch loop re-reads each key.  Supports counts (`3w'), the d/c/y
// operators with motions and their doubled forms (dd/cc/yy), find-char
// (f/F/t/T and ;/,), put (p/P), single-char edits (x/X/r/s/S/~), and one-level
// undo (u).

#include <algorithm>
#include <cctype>
#include <string>

#include "gnash/readline_internal.hpp"
#include "readline/keymaps.h"
#include "readline/readline.h"

extern "C" {
int rl_editing_mode = 1;  // 1 = emacs, 0 = vi
Keymap vi_insertion_keymap = nullptr;
Keymap vi_movement_keymap = nullptr;
}

namespace {

// ---- shared command-mode state --------------------------------------------
std::string vi_reg;             // unnamed register: delete/change/yank <-> put
std::string undo_line;          // one-level undo snapshot
int undo_point = 0;
bool undo_valid = false;
char find_type = 0, find_char = 0;  // last f/F/t/T, for `;'/`,'

void save_undo() {
  undo_line.assign(rl_line_buffer, static_cast<size_t>(rl_end));
  undo_point = rl_point;
  undo_valid = true;
}

bool is_word(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
bool is_space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

void del_range(int lo, int hi) {
  lo = std::max(lo, 0);
  hi = std::min(hi, rl_end);
  if (hi <= lo) return;
  vi_reg.assign(rl_line_buffer + lo, static_cast<size_t>(hi - lo));
  rl_delete_text(lo, hi);
  rl_point = lo;
}

void yank_range(int lo, int hi) {
  lo = std::max(lo, 0);
  hi = std::min(hi, rl_end);
  if (hi <= lo) return;
  vi_reg.assign(rl_line_buffer + lo, static_cast<size_t>(hi - lo));
}

// ---- word motions ---------------------------------------------------------
void word_forward(int count, bool big) {  // `w'/`W'
  for (int k = 0; k < count && rl_point < rl_end; k++) {
    int p = rl_point;
    if (big) {
      while (p < rl_end && !is_space(rl_line_buffer[p])) p++;
    } else if (is_word(rl_line_buffer[p])) {
      while (p < rl_end && is_word(rl_line_buffer[p])) p++;
    } else {
      while (p < rl_end && !is_word(rl_line_buffer[p]) && !is_space(rl_line_buffer[p])) p++;
    }
    while (p < rl_end && is_space(rl_line_buffer[p])) p++;
    rl_point = p;
  }
}

void word_backward(int count, bool big) {  // `b'/`B'
  for (int k = 0; k < count && rl_point > 0; k++) {
    int p = rl_point - 1;
    while (p > 0 && is_space(rl_line_buffer[p])) p--;
    if (p >= 0 && (big || is_word(rl_line_buffer[p]))) {
      while (p > 0 && !is_space(rl_line_buffer[p - 1]) && (big || is_word(rl_line_buffer[p - 1]))) p--;
    } else {
      while (p > 0 && !is_space(rl_line_buffer[p - 1]) && !is_word(rl_line_buffer[p - 1])) p--;
    }
    rl_point = p;
  }
}

void word_end(int count, bool big) {  // `e'/`E'
  for (int k = 0; k < count && rl_point < rl_end; k++) {
    int p = rl_point + 1;
    while (p < rl_end && is_space(rl_line_buffer[p])) p++;
    if (p < rl_end && (big || is_word(rl_line_buffer[p]))) {
      while (p + 1 < rl_end && !is_space(rl_line_buffer[p + 1]) && (big || is_word(rl_line_buffer[p + 1]))) p++;
    } else {
      while (p + 1 < rl_end && !is_space(rl_line_buffer[p + 1]) && !is_word(rl_line_buffer[p + 1])) p++;
    }
    rl_point = std::min(p, rl_end);
  }
}

// ---- find char (f/F/t/T) --------------------------------------------------
int find_col(char type, char ch, int count) {  // resulting column, or -1
  int p = rl_point;
  for (int k = 0; k < count; k++) {
    if (type == 'f' || type == 't') {
      int q = p + ((type == 't' && k == 0 && p + 1 < rl_end && rl_line_buffer[p + 1] == ch) ? 2 : 1);
      while (q < rl_end && rl_line_buffer[q] != ch) q++;
      if (q >= rl_end) return -1;
      p = q;
    } else {
      int q = p - ((type == 'T' && k == 0 && p - 1 >= 0 && rl_line_buffer[p - 1] == ch) ? 2 : 1);
      while (q >= 0 && rl_line_buffer[q] != ch) q--;
      if (q < 0) return -1;
      p = q;
    }
  }
  if (type == 't') p--;
  if (type == 'T') p++;
  return p;
}

// ---- a motion for an operator: move rl_point, report inclusivity ----------
bool operator_motion(int key, int count, bool &inclusive) {
  inclusive = false;
  switch (key) {
    case 'h': for (int k = 0; k < count && rl_point > 0; k++) rl_point--; return true;
    case 'l': case ' ': for (int k = 0; k < count && rl_point < rl_end; k++) rl_point++; return true;
    case 'w': word_forward(count, false); return true;
    case 'W': word_forward(count, true); return true;
    case 'b': word_backward(count, false); return true;
    case 'B': word_backward(count, true); return true;
    case 'e': word_end(count, false); inclusive = true; return true;
    case 'E': word_end(count, true); inclusive = true; return true;
    case '0': rl_point = 0; return true;
    case '^': rl_beg_of_line(1, key); return true;
    case '$': rl_point = rl_end; return true;
    case 'f': case 'F': case 't': case 'T': {
      int ch = rl_read_key();
      if (ch == EOF) return false;
      find_type = static_cast<char>(key);
      find_char = static_cast<char>(ch);
      int t = find_col(static_cast<char>(key), static_cast<char>(ch), count);
      if (t < 0) return false;
      rl_point = t;
      if (key == 'f' || key == 't') inclusive = true;
      return true;
    }
    default: return false;
  }
}

// ---- operators (d/c/y) ----------------------------------------------------
int vi_operator(int count, int key) {
  int c = rl_read_key();
  if (c == EOF) return 0;
  int mcount = 1;
  if (c > '0' && c <= '9') {  // count between operator and motion
    mcount = 0;
    while (c >= '0' && c <= '9') { mcount = mcount * 10 + (c - '0'); c = rl_read_key(); if (c == EOF) return 0; }
  }
  int total = count * mcount;
  int lo, hi;
  if (c == key) {  // dd / cc / yy: the whole line
    lo = 0; hi = rl_end;
  } else {
    int start = rl_point;
    bool inclusive = false;
    if (!operator_motion(c, total, inclusive)) { rl_point = start; return rl_ding(); }
    int end = rl_point;
    rl_point = start;
    if (inclusive) { if (end >= start) end++; else start++; }
    lo = std::min(start, end);
    hi = std::max(start, end);
  }
  save_undo();
  if (key == 'y') {
    yank_range(lo, hi);
    rl_point = lo;
  } else {
    del_range(lo, hi);
    if (key == 'c') rl_set_keymap(vi_insertion_keymap);  // cw/cc leave you inserting
  }
  return 0;
}

// ---- single-key edits -----------------------------------------------------
int vi_delete_char(int count, int /*key*/) {  // x
  if (rl_point >= rl_end) return rl_ding();
  save_undo();
  del_range(rl_point, std::min(rl_point + count, rl_end));
  return 0;
}
int vi_rubout(int count, int /*key*/) {  // X
  if (rl_point == 0) return rl_ding();
  save_undo();
  del_range(std::max(rl_point - count, 0), rl_point);
  return 0;
}
int vi_replace_char(int count, int /*key*/) {  // r
  int ch = rl_read_key();
  if (ch == EOF || rl_point + count > rl_end) return rl_ding();
  save_undo();
  int at = rl_point;
  del_range(at, at + count);
  std::string s(static_cast<size_t>(count), static_cast<char>(ch));
  rl_insert_text(s.c_str());
  rl_point = at + count - 1;
  return 0;
}
int vi_toggle_case(int count, int /*key*/) {  // ~
  if (rl_point >= rl_end) return rl_ding();
  save_undo();
  int n = std::min(count, rl_end - rl_point);
  for (int k = 0; k < n; k++) {
    char &c = rl_line_buffer[rl_point + k];
    if (std::islower(static_cast<unsigned char>(c))) c = static_cast<char>(std::toupper(c));
    else if (std::isupper(static_cast<unsigned char>(c))) c = static_cast<char>(std::tolower(c));
  }
  rl_point = std::min(rl_point + n, rl_end);
  return 0;
}

// ---- put (p/P) ------------------------------------------------------------
int vi_put_after(int count, int /*key*/) {
  if (vi_reg.empty()) return rl_ding();
  save_undo();
  if (rl_point < rl_end) rl_point++;
  int at = rl_point;
  for (int k = 0; k < count; k++) rl_insert_text(vi_reg.c_str());
  rl_point = std::max(at + static_cast<int>(vi_reg.size()) * count - 1, 0);
  return 0;
}
int vi_put_before(int count, int /*key*/) {
  if (vi_reg.empty()) return rl_ding();
  save_undo();
  int at = rl_point;
  for (int k = 0; k < count; k++) rl_insert_text(vi_reg.c_str());
  rl_point = std::max(at + static_cast<int>(vi_reg.size()) * count - 1, 0);
  return 0;
}

// ---- undo (single level, toggling like vi's `u') --------------------------
int vi_undo(int /*count*/, int /*key*/) {
  if (!undo_valid) return rl_ding();
  std::string cur(rl_line_buffer, static_cast<size_t>(rl_end));
  int curp = rl_point;
  rl_replace_line(undo_line.c_str(), 0);
  rl_point = std::min(undo_point, rl_end);
  undo_line = cur;
  undo_point = curp;
  return 0;
}

// ---- find (f/F/t/T) and repeat (;/,) as standalone motions ----------------
int vi_char_search(int count, int key) {
  int ch = rl_read_key();
  if (ch == EOF) return 0;
  find_type = static_cast<char>(key);
  find_char = static_cast<char>(ch);
  int t = find_col(static_cast<char>(key), static_cast<char>(ch), count);
  if (t < 0) return rl_ding();
  rl_point = t;
  return 0;
}
int vi_char_search_repeat(int count, int key) {
  if (!find_type) return rl_ding();
  char type = find_type;
  if (key == ',') {  // reverse direction
    type = (type == 'f') ? 'F' : (type == 'F') ? 'f' : (type == 't') ? 'T' : 't';
  }
  int t = find_col(type, find_char, count);
  if (t < 0) return rl_ding();
  rl_point = t;
  return 0;
}

// ---- count-aware plain motions --------------------------------------------
int vi_word(int count, int /*key*/) { word_forward(count, false); return 0; }
int vi_word_big(int count, int /*key*/) { word_forward(count, true); return 0; }
int vi_back(int count, int /*key*/) { word_backward(count, false); return 0; }
int vi_back_big(int count, int /*key*/) { word_backward(count, true); return 0; }
int vi_eword(int count, int /*key*/) { word_end(count, false); return 0; }
int vi_eword_big(int count, int /*key*/) { word_end(count, true); return 0; }

// ---- mode entries ---------------------------------------------------------
int vi_movement_mode(int /*count*/, int /*key*/) {  // ESC
  rl_set_keymap(vi_movement_keymap);
  if (rl_point > 0) rl_point--;
  return 0;
}
int vi_insert_mode(int /*count*/, int /*key*/) { rl_set_keymap(vi_insertion_keymap); return 0; }
int vi_append_mode(int /*count*/, int /*key*/) {
  if (rl_point < rl_end) rl_point++;
  rl_set_keymap(vi_insertion_keymap);
  return 0;
}
int vi_insert_beg(int count, int key) { rl_beg_of_line(count, key); rl_set_keymap(vi_insertion_keymap); return 0; }
int vi_append_eol(int count, int key) { rl_end_of_line(count, key); rl_set_keymap(vi_insertion_keymap); return 0; }
int vi_change_to_eol(int count, int key) {  // C
  save_undo();
  vi_reg.assign(rl_line_buffer + rl_point, static_cast<size_t>(rl_end - rl_point));
  rl_kill_line(count, key);
  rl_set_keymap(vi_insertion_keymap);
  return 0;
}
int vi_delete_to_eol(int count, int key) {  // D
  save_undo();
  vi_reg.assign(rl_line_buffer + rl_point, static_cast<size_t>(rl_end - rl_point));
  return rl_kill_line(count, key);
}
int vi_subst_char(int count, int /*key*/) {  // s
  save_undo();
  del_range(rl_point, std::min(rl_point + count, rl_end));
  rl_set_keymap(vi_insertion_keymap);
  return 0;
}
int vi_subst_line(int /*count*/, int /*key*/) {  // S
  save_undo();
  vi_reg.assign(rl_line_buffer, static_cast<size_t>(rl_end));
  rl_delete_text(0, rl_end);
  rl_point = 0;
  rl_set_keymap(vi_insertion_keymap);
  return 0;
}

void bind_func(Keymap m, int c, rl_command_func_t *f) {
  m[c].type = ISFUNC;
  m[c].function = f;
  m[c].kmap = nullptr;
}

}  // namespace

// ---- mode switches (public) -----------------------------------------------

extern "C" int rl_vi_editing_mode(int /*count*/, int /*key*/) {
  rl_editing_mode = 0;
  gnash::readline::build_vi_keymaps();
  rl_set_keymap(vi_insertion_keymap);
  return 0;
}

extern "C" int rl_emacs_editing_mode(int /*count*/, int /*key*/) {
  rl_editing_mode = 1;
  rl_set_keymap(rl_get_keymap_by_name("emacs"));
  return 0;
}

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
  // counts: 1-9 start a numeric argument that consumes further digits and then
  // dispatches the terminating command in this keymap with the accumulated count.
  for (int d = '1'; d <= '9'; d++) bind_func(mov, d, rl_digit_argument);
  // motions
  bind_func(mov, 'h', rl_backward_char); bind_func(mov, 0x08, rl_backward_char); bind_func(mov, 0x7f, rl_backward_char);
  bind_func(mov, 'l', rl_forward_char);  bind_func(mov, ' ', rl_forward_char);
  bind_func(mov, '0', rl_beg_of_line);
  bind_func(mov, '^', rl_beg_of_line);
  bind_func(mov, '$', rl_end_of_line);
  bind_func(mov, 'w', vi_word);   bind_func(mov, 'W', vi_word_big);
  bind_func(mov, 'b', vi_back);   bind_func(mov, 'B', vi_back_big);
  bind_func(mov, 'e', vi_eword);  bind_func(mov, 'E', vi_eword_big);
  bind_func(mov, 'f', vi_char_search); bind_func(mov, 'F', vi_char_search);
  bind_func(mov, 't', vi_char_search); bind_func(mov, 'T', vi_char_search);
  bind_func(mov, ';', vi_char_search_repeat); bind_func(mov, ',', vi_char_search_repeat);
  // operators
  bind_func(mov, 'd', vi_operator); bind_func(mov, 'c', vi_operator); bind_func(mov, 'y', vi_operator);
  // edits
  bind_func(mov, 'x', vi_delete_char); bind_func(mov, 'X', vi_rubout);
  bind_func(mov, 'r', vi_replace_char); bind_func(mov, '~', vi_toggle_case);
  bind_func(mov, 'D', vi_delete_to_eol); bind_func(mov, 'C', vi_change_to_eol);
  bind_func(mov, 's', vi_subst_char); bind_func(mov, 'S', vi_subst_line);
  bind_func(mov, 'p', vi_put_after); bind_func(mov, 'P', vi_put_before);
  bind_func(mov, 'u', vi_undo);
  // insert entries
  bind_func(mov, 'i', vi_insert_mode); bind_func(mov, 'a', vi_append_mode);
  bind_func(mov, 'I', vi_insert_beg);  bind_func(mov, 'A', vi_append_eol);
  // history + accept
  bind_func(mov, 'k', rl_get_previous_history); bind_func(mov, 'j', rl_get_next_history);
  bind_func(mov, '-', rl_get_previous_history); bind_func(mov, '+', rl_get_next_history);
  bind_func(mov, '\r', rl_newline); bind_func(mov, '\n', rl_newline);

  vi_insertion_keymap = ins;
  vi_movement_keymap = mov;
}

}  // namespace gnash::readline
