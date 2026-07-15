// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// misc.cpp -- the smaller bindable commands from bash 5.3 lib/readline's
// misc.c / util.c / text.c / macro.c that don't belong to a bigger subsystem:
// undo and revert-line, the mark, keyboard macros, quoted/tab insert,
// tilde-expand, insert-comment, clear-display, and execute-named-command.
//
// Undo here is snapshot-based: the dispatch loop (readline.cpp) records the
// line before every buffer-changing command, and undo pops snapshots.  GNU
// readline keeps a finer-grained undo list; command-level granularity matches
// what a keystroke does, which is what C-_ users expect.

#include <cctype>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <string>
#include <vector>

#include "gnash/readline_internal.hpp"
#include "gnash/sh/xmalloc.hpp"
#include "readline/readline.h"
#include "readline/tilde.h"

namespace {

struct UndoState {
  std::string line;
  int point;
};

std::vector<UndoState> undo_stack;
unsigned undo_gen = 0;

void restore(const UndoState &u) {
  rl_replace_line(u.line.c_str(), 0);
  rl_point = u.point <= rl_end ? u.point : rl_end;
}

}  // namespace

namespace gnash::readline {

void undo_clear() {
  undo_stack.clear();
  undo_gen++;
}

unsigned undo_generation() { return undo_gen; }

void undo_push(const std::string &line, int point) {
  undo_stack.push_back({line, point});
  if (undo_stack.size() > 200) undo_stack.erase(undo_stack.begin());
}

}  // namespace gnash::readline

// ---- undo / revert-line ----------------------------------------------------

extern "C" int rl_undo_command(int count, int /*key*/) {
  if (count < 0) return 0;
  while (count-- > 0) {
    if (undo_stack.empty()) return rl_ding();
    restore(undo_stack.back());
    undo_stack.pop_back();
  }
  return 0;
}

extern "C" int rl_revert_line(int /*count*/, int /*key*/) {
  if (undo_stack.empty()) return 0;  // no edits: already the original line
  restore(undo_stack.front());
  undo_stack.clear();
  return 0;
}

// ---- abort and the mark ----------------------------------------------------

extern "C" int rl_abort(int /*count*/, int /*key*/) { return rl_ding(); }

extern "C" int rl_set_mark(int count, int /*key*/) {
  int at = rl_explicit_arg ? count : rl_point;
  if (at < 0 || at > rl_end) return rl_ding();
  rl_mark = at;
  return 0;
}

extern "C" int rl_exchange_point_and_mark(int /*count*/, int /*key*/) {
  if (rl_mark > rl_end) rl_mark = rl_end;
  int t = rl_point;
  rl_point = rl_mark;
  rl_mark = t;
  return 0;
}

// ---- keyboard macros (C-x ( / C-x ) / C-x e) --------------------------------

namespace {
std::string last_macro;  // the last completed macro definition
}

extern "C" int rl_start_kbd_macro(int /*count*/, int /*key*/) {
  if (gnash::readline::macro_recording) return rl_ding();
  gnash::readline::macro_keys.clear();
  gnash::readline::macro_recording = true;
  return 0;
}

extern "C" int rl_end_kbd_macro(int /*count*/, int /*key*/) {
  if (!gnash::readline::macro_recording) return rl_ding();
  gnash::readline::macro_recording = false;
  // The `C-x )' that ended the definition was recorded on its way here; it is
  // not part of the macro.
  std::string &keys = gnash::readline::macro_keys;
  if (keys.size() >= 2) keys.erase(keys.size() - 2);
  last_macro = keys;
  return 0;
}

extern "C" int rl_call_last_kbd_macro(int count, int /*key*/) {
  if (last_macro.empty()) return rl_ding();
  if (gnash::readline::macro_recording) return rl_ding();  // no recursive replay
  while (count-- > 0) gnash::readline::stuff_input(last_macro);
  return 0;
}

// ---- literal inserts ---------------------------------------------------------

extern "C" int rl_quoted_insert(int count, int /*key*/) {
  int c = rl_read_key();
  if (c == EOF) return rl_ding();
  return rl_insert(count, c);
}

extern "C" int rl_tab_insert(int count, int /*key*/) { return rl_insert(count, '\t'); }

// ---- tilde expansion (M-&, M-~) ---------------------------------------------

extern "C" int rl_tilde_expand(int /*count*/, int /*key*/) {
  // Find the whitespace-delimited word around point.
  int start = rl_point;
  int end = rl_point;
  if (start == rl_end && start > 0) start--;
  while (start > 0 && !std::isspace(static_cast<unsigned char>(rl_line_buffer[start - 1])))
    start--;
  while (end < rl_end && !std::isspace(static_cast<unsigned char>(rl_line_buffer[end])))
    end++;
  if (end <= start || rl_line_buffer[start] != '~') return rl_ding();

  std::string word(rl_line_buffer + start, static_cast<size_t>(end - start));
  char *expanded = tilde_expand(word.c_str());
  if (expanded == nullptr) return rl_ding();
  rl_delete_text(start, end);
  rl_point = start;
  rl_insert_text(expanded);
  gnash::sh::xfree(expanded);
  return 0;
}

// ---- insert-comment (M-#) -----------------------------------------------------

extern "C" int rl_insert_comment(int /*count*/, int key) {
  static const char comment[] = "#";
  rl_beg_of_line(1, key);
  if (rl_explicit_arg == 0) {
    rl_insert_text(comment);
  } else {
    // With an argument, toggle: remove the comment prefix if present.
    size_t clen = sizeof(comment) - 1;
    if (static_cast<size_t>(rl_end) >= clen &&
        std::strncmp(rl_line_buffer, comment, clen) == 0)
      rl_delete_text(0, static_cast<int>(clen));
    else
      rl_insert_text(comment);
  }
  rl_redisplay();
  return rl_newline(1, '\n');
}

// ---- clear-display (M-C-l) ----------------------------------------------------

extern "C" int rl_clear_display(int /*count*/, int /*key*/) {
  FILE *o = rl_outstream ? rl_outstream : stdout;
  std::fputs("\033[H\033[2J\033[3J", o);  // home + clear screen + clear scrollback
  rl_redisplay();
  return 0;
}

// ---- do-lowercase-version (dispatch handles it; this is a sentinel) -----------

extern "C" int rl_do_lowercase_version(int /*count*/, int /*key*/) { return 0; }

// ---- re-read-init-file (C-x C-r) ----------------------------------------------

extern "C" int rl_re_read_init_file(int /*count*/, int /*key*/) {
  return rl_read_init_file(nullptr);
}

// ---- execute-named-command (M-x) -----------------------------------------------

extern "C" int rl_execute_named_command(int count, int /*key*/) {
  FILE *o = rl_outstream ? rl_outstream : stdout;
  bool tty = isatty(fileno(o));
  std::string name;

  for (;;) {
    if (tty) {
      std::fprintf(o, "\r!%s\033[K", name.c_str());
      std::fflush(o);
    }
    int c = rl_read_key();
    if (c == EOF || c == 0x07 || c == 0x1b) return rl_ding();  // C-g/ESC: abort
    if (c == '\r' || c == '\n') break;
    if (c == 0x7f || c == 0x08) {
      if (!name.empty()) name.pop_back();
      continue;
    }
    if (c >= ' ' && c < 0x7f) name += static_cast<char>(c);
  }

  rl_command_func_t *fn = rl_named_function(name.c_str());
  if (fn == nullptr) return rl_ding();
  return fn(count, 0);
}
