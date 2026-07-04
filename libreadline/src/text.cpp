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

// ---- case ----------------------------------------------------------------

extern "C" int rl_upcase_word(int count, int /*key*/) { return change_case(count, kUp); }
extern "C" int rl_downcase_word(int count, int /*key*/) { return change_case(count, kDown); }
extern "C" int rl_capitalize_word(int count, int /*key*/) { return change_case(count, kCap); }
