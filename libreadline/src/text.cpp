// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// text.cpp -- bindable editing commands operating on the Readline line buffer.
//
// Ported behaviour from bash 5.3 lib/readline/text.c and kill.c.  Each function
// has the classic (count, key) signature and mutates rl_line_buffer / rl_point
// / rl_end.  Redisplay is intentionally not called here; the tty loop will
// refresh after dispatching a command.

#include <cctype>
#include <string>

#include "gnash/readline_internal.hpp"
#include "gnash/sh/xmalloc.hpp"
#include "readline/history.h"
#include "readline/readline.h"

namespace {

inline int wordchar(int c) { return std::isalnum(static_cast<unsigned char>(c)); }

// A non-kill command breaks kill accumulation.
inline void breaks_kill() { gnash::readline::last_command_was_kill = 0; }

// Index one word forward from P (skip non-word, then word); COUNT times.
int forward_word_from(int p, int count) {
  while (count-- > 0) {
    while (p < rl_end && !wordchar(rl_line_buffer[p])) p++;
    while (p < rl_end && wordchar(rl_line_buffer[p])) p++;
  }
  return p;
}

// Index one word backward from P; COUNT times.
int backward_word_from(int p, int count) {
  while (count-- > 0) {
    while (p > 0 && !wordchar(rl_line_buffer[p - 1])) p--;
    while (p > 0 && wordchar(rl_line_buffer[p - 1])) p--;
  }
  return p;
}

enum CaseMode { kUp, kDown, kCap };

int change_case(int count, CaseMode mode) {
  breaks_kill();
  if (count < 0) count = 0;
  int p = rl_point;
  while (count-- > 0) {
    while (p < rl_end && !wordchar(rl_line_buffer[p])) p++;
    bool first = true;
    while (p < rl_end && wordchar(rl_line_buffer[p])) {
      int c = static_cast<unsigned char>(rl_line_buffer[p]);
      if (mode == kUp)
        c = std::toupper(c);
      else if (mode == kDown)
        c = std::tolower(c);
      else
        c = first ? std::toupper(c) : std::tolower(c);
      rl_line_buffer[p] = static_cast<char>(c);
      first = false;
      p++;
    }
  }
  rl_point = p;
  return 0;
}

}  // namespace

// ---- insertion -----------------------------------------------------------

extern "C" int rl_insert(int count, int c) {
  breaks_kill();
  if (count <= 0) return 0;
  std::string s(static_cast<size_t>(count), static_cast<char>(c));
  rl_insert_text(s.c_str());
  return 0;
}

// ---- motion --------------------------------------------------------------

extern "C" int rl_forward_char(int count, int key) {
  breaks_kill();
  if (count < 0) return rl_backward_char(-count, key);
  rl_point += count;
  if (rl_point > rl_end) rl_point = rl_end;
  return 0;
}

extern "C" int rl_backward_char(int count, int key) {
  breaks_kill();
  if (count < 0) return rl_forward_char(-count, key);
  rl_point -= count;
  if (rl_point < 0) rl_point = 0;
  return 0;
}

extern "C" int rl_beg_of_line(int /*count*/, int /*key*/) {
  breaks_kill();
  rl_point = 0;
  return 0;
}

extern "C" int rl_end_of_line(int /*count*/, int /*key*/) {
  breaks_kill();
  rl_point = rl_end;
  return 0;
}

extern "C" int rl_forward_word(int count, int key) {
  breaks_kill();
  if (count < 0) return rl_backward_word(-count, key);
  rl_point = forward_word_from(rl_point, count);
  return 0;
}

extern "C" int rl_backward_word(int count, int key) {
  breaks_kill();
  if (count < 0) return rl_forward_word(-count, key);
  rl_point = backward_word_from(rl_point, count);
  return 0;
}

// ---- deletion ------------------------------------------------------------

extern "C" int rl_delete(int count, int /*key*/) {
  breaks_kill();
  if (count < 0) count = -count;
  if (rl_point == rl_end) return rl_ding();
  int to = rl_point + count;
  if (to > rl_end) to = rl_end;
  rl_delete_text(rl_point, to);
  return 0;
}

extern "C" int rl_rubout(int count, int /*key*/) {
  breaks_kill();
  if (count < 0) count = -count;
  if (rl_point == 0) return rl_ding();
  int from = rl_point - count;
  if (from < 0) from = 0;
  rl_delete_text(from, rl_point);
  rl_point = from;
  return 0;
}

// ---- kills (accumulate into the kill ring) -------------------------------

extern "C" int rl_kill_line(int /*count*/, int /*key*/) {
  gnash::readline::kill_text(rl_point, rl_end, +1);
  return 0;
}

extern "C" int rl_backward_kill_line(int /*count*/, int /*key*/) {
  gnash::readline::kill_text(0, rl_point, -1);
  return 0;
}

extern "C" int rl_unix_line_discard(int count, int key) {
  return rl_backward_kill_line(count, key);
}

extern "C" int rl_kill_word(int count, int key) {
  if (count < 0) return rl_backward_kill_word(-count, key);
  int to = forward_word_from(rl_point, count);
  gnash::readline::kill_text(rl_point, to, +1);
  return 0;
}

extern "C" int rl_backward_kill_word(int count, int key) {
  if (count < 0) return rl_kill_word(-count, key);
  int from = backward_word_from(rl_point, count);
  gnash::readline::kill_text(from, rl_point, -1);
  return 0;
}

extern "C" int rl_unix_word_rubout(int /*count*/, int /*key*/) {
  int p = rl_point;
  while (p > 0 && std::isspace(static_cast<unsigned char>(rl_line_buffer[p - 1]))) p--;
  while (p > 0 && !std::isspace(static_cast<unsigned char>(rl_line_buffer[p - 1]))) p--;
  gnash::readline::kill_text(p, rl_point, -1);
  return 0;
}

// ---- yank ----------------------------------------------------------------

extern "C" int rl_yank(int /*count*/, int /*key*/) {
  breaks_kill();
  const std::string *k = gnash::readline::current_kill();
  if (k == nullptr) return rl_ding();
  rl_mark = rl_point;
  rl_insert_text(k->c_str());
  return 0;
}

extern "C" int rl_yank_pop(int /*count*/, int /*key*/) {
  breaks_kill();
  // Only meaningful immediately after a yank (or another yank-pop): the text
  // between mark and point is the previous yank, which we replace with the
  // next-older kill-ring entry.
  if (rl_last_func != rl_yank && rl_last_func != rl_yank_pop) return rl_ding();
  const std::string *k = gnash::readline::rotate_kill();
  if (k == nullptr) return rl_ding();
  int start = rl_mark < rl_point ? rl_mark : rl_point;
  int end = rl_mark < rl_point ? rl_point : rl_mark;
  rl_delete_text(start, end);
  rl_point = rl_mark = start;
  rl_insert_text(k->c_str());
  return 0;
}

namespace {

// Insert the COUNTth word of a previous history line at point, skipping
// HISTORY_SKIP lines back before "the previous line".  COUNT may be the magic
// '$' (take the last word), which history_arg_extract() understands.
int yank_nth_arg_internal(int count, int history_skip) {
  int pos = where_history();
  HIST_ENTRY *entry = nullptr;
  for (int i = 0; i <= history_skip; i++) entry = previous_history();
  history_set_pos(pos);
  if (entry == nullptr) return rl_ding();

  char *arg = history_arg_extract(count, count, entry->line);
  if (arg == nullptr || *arg == '\0') {
    gnash::sh::xfree(arg);
    return rl_ding();
  }
  rl_mark = rl_point;
  rl_insert_text(arg);
  gnash::sh::xfree(arg);
  return 0;
}

}  // namespace

extern "C" int rl_yank_nth_arg(int count, int /*key*/) {
  breaks_kill();
  return yank_nth_arg_internal(count, 0);
}

extern "C" int rl_yank_last_arg(int count, int /*key*/) {
  // Successive invocations cycle back through history, replacing the argument
  // inserted by the previous press (bash tracks this with an undo group; we
  // remember the inserted span instead).
  static int history_skip = 0;
  static int explicit_arg_p = 0;
  static int count_passed = 1;
  static int direction = 1;
  static int inserted_at = -1;
  static int inserted_len = 0;

  breaks_kill();
  if (rl_last_func != rl_yank_last_arg) {
    history_skip = 0;
    explicit_arg_p = rl_explicit_arg;
    count_passed = count;
    direction = 1;
    inserted_at = -1;
    inserted_len = 0;
  } else {
    if (inserted_at >= 0 && inserted_len > 0 &&
        inserted_at + inserted_len <= rl_end) {
      rl_delete_text(inserted_at, inserted_at + inserted_len);
      rl_point = inserted_at;
    }
    if (count < 0) direction = -direction;
    history_skip += direction;
    if (history_skip < 0) history_skip = 0;
  }

  int before = rl_point;
  int r = yank_nth_arg_internal(explicit_arg_p ? count_passed : '$', history_skip);
  if (r == 0) {
    inserted_at = before;
    inserted_len = rl_point - before;
  } else {
    inserted_at = -1;
    inserted_len = 0;
  }
  return r;
}

// ---- transpose -----------------------------------------------------------

extern "C" int rl_transpose_chars(int /*count*/, int /*key*/) {
  breaks_kill();
  if (rl_point == 0 || rl_end < 2) return rl_ding();
  if (rl_point == rl_end) {
    char t = rl_line_buffer[rl_point - 1];
    rl_line_buffer[rl_point - 1] = rl_line_buffer[rl_point - 2];
    rl_line_buffer[rl_point - 2] = t;
  } else {
    char t = rl_line_buffer[rl_point];
    rl_line_buffer[rl_point] = rl_line_buffer[rl_point - 1];
    rl_line_buffer[rl_point - 1] = t;
    rl_point++;
  }
  return 0;
}

// ---- character search (C-], M-C-]) -----------------------------------------

namespace {

// Move point to the COUNTth occurrence of the next typed character, searching
// in DIR (+1 forward from point+1, -1 backward from point-1).
int char_search_internal(int count, int dir) {
  int ch = rl_read_key();
  if (ch == EOF) return rl_ding();
  if (count < 0) {
    count = -count;
    dir = -dir;
  }
  int p = rl_point;
  while (count-- > 0) {
    int q = p + dir;
    while (q >= 0 && q < rl_end && rl_line_buffer[q] != ch) q += dir;
    if (q < 0 || q >= rl_end) return rl_ding();
    p = q;
  }
  rl_point = p;
  return 0;
}

}  // namespace

extern "C" int rl_char_search(int count, int /*key*/) {
  breaks_kill();
  return char_search_internal(count, +1);
}

extern "C" int rl_backward_char_search(int count, int /*key*/) {
  breaks_kill();
  return char_search_internal(count, -1);
}

// ---- transpose-words (M-t) --------------------------------------------------

extern "C" int rl_transpose_words(int count, int key) {
  breaks_kill();
  if (count < 0) count = 1;
  while (count-- > 0) {
    int orig = rl_point;
    rl_forward_word(1, key);
    int w2_end = rl_point;
    rl_backward_word(1, key);
    int w2_beg = rl_point;
    rl_backward_word(1, key);
    int w1_beg = rl_point;
    rl_forward_word(1, key);
    int w1_end = rl_point;
    if (w1_beg == w2_beg || w2_beg < w1_end) {
      rl_point = orig;
      return rl_ding();
    }
    std::string w1(rl_line_buffer + w1_beg, static_cast<size_t>(w1_end - w1_beg));
    std::string sep(rl_line_buffer + w1_end, static_cast<size_t>(w2_beg - w1_end));
    std::string w2(rl_line_buffer + w2_beg, static_cast<size_t>(w2_end - w2_beg));
    rl_delete_text(w1_beg, w2_end);
    rl_point = w1_beg;
    rl_insert_text((w2 + sep + w1).c_str());  // leaves point after the swap
  }
  return 0;
}

// ---- delete-horizontal-space (M-\) -------------------------------------------

extern "C" int rl_delete_horizontal_space(int /*count*/, int /*key*/) {
  breaks_kill();
  int start = rl_point;
  while (start > 0 && std::isblank(static_cast<unsigned char>(rl_line_buffer[start - 1])))
    start--;
  int end = rl_point;
  while (end < rl_end && std::isblank(static_cast<unsigned char>(rl_line_buffer[end])))
    end++;
  if (end <= start) return 0;
  rl_delete_text(start, end);
  rl_point = start;
  return 0;
}

// ---- case ----------------------------------------------------------------

extern "C" int rl_upcase_word(int count, int /*key*/) { return change_case(count, kUp); }
extern "C" int rl_downcase_word(int count, int /*key*/) { return change_case(count, kDown); }
extern "C" int rl_capitalize_word(int count, int /*key*/) { return change_case(count, kCap); }
