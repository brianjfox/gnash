// state.cpp -- Readline editing state, line-buffer primitives, and kill ring.
//
// Mirrors the relevant globals and helpers from bash 5.3 lib/readline
// (readline.c / text.c / util.c / kill.c).  The line buffer is a malloc'd,
// NUL-terminated char array exposed directly as rl_line_buffer so the classic
// API and future redisplay see exactly what readline expects.

#include <cstring>
#include <string>
#include <vector>

#include "gnash/readline_internal.hpp"
#include "gnash/sh/xmalloc.hpp"
#include "readline/readline.h"

using gnash::sh::xfree;
using gnash::sh::xmalloc;
using gnash::sh::xrealloc;

// ---- exported editing state ----------------------------------------------
extern "C" {
char *rl_line_buffer = nullptr;
int rl_point = 0;
int rl_end = 0;
int rl_mark = 0;
int rl_done = 0;
}

namespace gnash::readline {

int line_buffer_len = 0;  // capacity of rl_line_buffer

// Kill ring: a small ring of killed text.  Consecutive kill commands
// accumulate into the newest entry (forward kills append, backward kills
// prepend), matching emacs/readline behaviour.
static std::vector<std::string> kill_ring;
static constexpr int kMaxKills = 10;
int last_command_was_kill = 0;

void maybe_init_line() {
  if (rl_line_buffer == nullptr) {
    line_buffer_len = 64;
    rl_line_buffer = static_cast<char *>(xmalloc(static_cast<size_t>(line_buffer_len)));
    rl_line_buffer[0] = '\0';
    rl_point = rl_end = 0;
  }
}

}  // namespace gnash::readline

using namespace gnash::readline;

extern "C" void rl_extend_line_buffer(int len) {
  maybe_init_line();
  if (len + 1 <= line_buffer_len) return;
  while (len + 1 > line_buffer_len) line_buffer_len += (line_buffer_len < 512) ? 64 : 256;
  rl_line_buffer =
      static_cast<char *>(xrealloc(rl_line_buffer, static_cast<size_t>(line_buffer_len)));
}

extern "C" int rl_insert_text(const char *text) {
  maybe_init_line();
  if (text == nullptr) return 0;
  int l = static_cast<int>(std::strlen(text));
  if (l == 0) return 0;

  rl_extend_line_buffer(rl_end + l);
  // Shift the tail [rl_point, rl_end) right to make room.
  std::memmove(rl_line_buffer + rl_point + l, rl_line_buffer + rl_point,
               static_cast<size_t>(rl_end - rl_point));
  std::memcpy(rl_line_buffer + rl_point, text, static_cast<size_t>(l));
  rl_point += l;
  rl_end += l;
  rl_line_buffer[rl_end] = '\0';
  return l;
}

extern "C" int rl_delete_text(int from, int to) {
  maybe_init_line();
  if (from > to) {
    int t = from;
    from = to;
    to = t;
  }
  if (from < 0) from = 0;
  if (to > rl_end) to = rl_end;
  int diff = to - from;
  if (diff <= 0) return 0;

  std::memmove(rl_line_buffer + from, rl_line_buffer + to,
               static_cast<size_t>(rl_end - to));
  rl_end -= diff;
  rl_line_buffer[rl_end] = '\0';
  return diff;
}

extern "C" void rl_replace_line(const char *text, int /*clear_undo*/) {
  maybe_init_line();
  int len = text ? static_cast<int>(std::strlen(text)) : 0;
  rl_extend_line_buffer(len);
  if (len)
    std::memcpy(rl_line_buffer, text, static_cast<size_t>(len));
  rl_line_buffer[len] = '\0';
  rl_end = len;
  if (rl_point > rl_end) rl_point = rl_end;
  if (rl_mark > rl_end) rl_mark = rl_end;
}

extern "C" int rl_ding(void) { return -1; }

extern "C" void rl_reset_last_command(void) { last_command_was_kill = 0; }

namespace gnash::readline {

// Add killed text (the half-open range [from,to)) to the kill ring, then remove
// it from the line.  DIR > 0 means a forward kill (append to the current entry
// on accumulation), DIR < 0 means backward (prepend).
void kill_text(int from, int to, int dir) {
  maybe_init_line();
  if (from > to) {
    int t = from;
    from = to;
    to = t;
  }
  if (from < 0) from = 0;
  if (to > rl_end) to = rl_end;
  if (to <= from) return;

  std::string slice(rl_line_buffer + from, static_cast<size_t>(to - from));

  if (last_command_was_kill && !kill_ring.empty()) {
    if (dir < 0)
      kill_ring.back() = slice + kill_ring.back();
    else
      kill_ring.back() += slice;
  } else {
    kill_ring.push_back(slice);
    if (static_cast<int>(kill_ring.size()) > kMaxKills) kill_ring.erase(kill_ring.begin());
  }

  rl_delete_text(from, to);
  if (rl_point > from) rl_point = from;
  last_command_was_kill = 1;
}

const std::string *current_kill() {
  if (kill_ring.empty()) return nullptr;
  return &kill_ring.back();
}

void reset_kill_ring() {
  kill_ring.clear();
  last_command_was_kill = 0;
}

}  // namespace gnash::readline
