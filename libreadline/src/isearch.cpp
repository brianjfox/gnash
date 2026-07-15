// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// isearch.cpp -- incremental history search (C-r / C-s).
//
// A functional port of the interaction in bash 5.3 lib/readline/isearch.c:
// read keys, extend/trim the search string, walk the history for a line that
// contains it, and show it with the "(reverse-i-search)`str':" prompt.  RET
// accepts, C-g aborts (restoring the original line), C-r/C-s repeat, DEL trims.

#include <cstdio>
#include <unistd.h>
#include <cstring>
#include <string>

#include "gnash/readline_internal.hpp"
#include "readline/history.h"
#include "readline/readline.h"

namespace {

// First history index (0-based) at or beyond FROM, moving by DIR, whose line
// contains SS.  Returns -1 if none.
int search_history(const std::string &ss, int from, int dir) {
  for (int i = from; i >= 0 && i < history_length; i += dir) {
    HIST_ENTRY *e = history_get(history_base + i);
    if (e && e->line && std::strstr(e->line, ss.c_str())) return i;
  }
  return -1;
}

void show(int dir, const std::string &ss) {
  FILE *o = rl_outstream ? rl_outstream : stdout;
  const char *pfx = (dir < 0) ? "(reverse-i-search)`" : "(i-search)`";
  std::fprintf(o, "\r%s%s': %s\033[K", pfx, ss.c_str(), rl_line_buffer);
  // Leave the cursor at the match (rl_point) within the shown line, not at its
  // end, by backing up over the trailing characters.
  for (int i = rl_end; i > rl_point; i--) std::fputc('\b', o);
  std::fflush(o);
}

int isearch(int dir) {
  std::string orig(rl_line_buffer, static_cast<size_t>(rl_end));
  std::string ss;
  int matchpos = -1;  // history index currently displayed, -1 = none yet
  bool tty = rl_outstream != nullptr && isatty(fileno(rl_outstream));

  for (;;) {
    if (tty) show(dir, ss);
    int c = rl_read_key();

    if (c == EOF) return 0;
    if (c == 0x07) {  // C-g: abort
      rl_replace_line(orig.c_str(), 1);
      rl_point = rl_end;
      return 0;
    }
    if (c == '\r' || c == '\n') {  // accept
      if (matchpos >= 0) history_set_pos(matchpos);
      rl_done = 1;
      return 0;
    }

    int start;
    if (c == 0x12) {  // C-r: next older match
      dir = -1;
      start = (matchpos < 0) ? history_length - 1 : matchpos - 1;
    } else if (c == 0x13) {  // C-s: next newer match
      dir = 1;
      start = (matchpos < 0) ? 0 : matchpos + 1;
    } else if (c == 0x7f || c == 0x08) {  // DEL/backspace: trim
      if (!ss.empty()) ss.pop_back();
      start = (dir < 0) ? history_length - 1 : 0;
      matchpos = -1;
    } else if (c >= 0x20 && c < 0x7f) {  // extend the search string
      ss.push_back(static_cast<char>(c));
      start = (matchpos < 0) ? ((dir < 0) ? history_length - 1 : 0) : matchpos;
    } else {
      // Any other key terminates the search, leaving the found line; the key
      // itself is executed as a normal command (as GNU readline does).  The
      // history position moves to the match so commands like C-o and C-p
      // continue from it.
      if (matchpos >= 0) history_set_pos(matchpos);
      gnash::readline::stuff_input(std::string(1, static_cast<char>(c)));
      return 0;
    }

    int f = search_history(ss, start, (dir < 0) ? -1 : 1);
    if (f >= 0) {
      matchpos = f;
      HIST_ENTRY *e = history_get(history_base + f);
      rl_replace_line(e->line, 1);
      // Put the cursor at the start of the match, not the end of the line: the
      // last occurrence for a reverse search, the first for a forward one (bash).
      std::string line = e->line;
      std::string::size_type pos =
          ss.empty() ? std::string::npos : (dir < 0 ? line.rfind(ss) : line.find(ss));
      rl_point = (pos != std::string::npos) ? static_cast<int>(pos) : rl_end;
    } else if (!ss.empty()) {
      rl_ding();
    }
  }
}

}  // namespace

extern "C" int rl_reverse_search_history(int /*count*/, int /*key*/) {
  return isearch(-1);
}

extern "C" int rl_forward_search_history(int /*count*/, int /*key*/) {
  return isearch(1);
}

// ---- non-incremental search (M-n / M-p, vi / ? n N) ------------------------
//
// Reads a search string on a minibuffer line introduced by PCHAR, then finds
// the nearest history line containing it in DIR (-1 older, +1 newer).  The
// string is remembered so the `again' entries (vi `n'/`N') can repeat it.

namespace {

std::string nsearch_string;

// Read the search string; false if the user aborted.
bool nsearch_read(int pchar) {
  FILE *o = (rl_outstream && isatty(fileno(rl_outstream))) ? rl_outstream : nullptr;
  std::string s;
  for (;;) {
    if (o) {
      std::fprintf(o, "\r%s%c%s\033[K", rl_prompt ? rl_prompt : "", pchar, s.c_str());
      std::fflush(o);
    }
    int c = rl_read_key();
    if (c == EOF || c == 0x07 || c == 0x1b) return false;  // C-g/ESC: abort
    if (c == '\r' || c == '\n') break;
    if (c == 0x7f || c == 0x08) {
      if (!s.empty()) s.pop_back();
      continue;
    }
    if (c >= ' ') s.push_back(static_cast<char>(c));
  }
  if (!s.empty()) nsearch_string = s;  // empty input repeats the last search
  return true;
}

int nsearch_do(int dir) {
  if (nsearch_string.empty()) return rl_ding();
  int from = where_history() + dir;
  int found = search_history(nsearch_string, from, dir);
  if (found < 0) return rl_ding();
  HIST_ENTRY *e = history_get(history_base + found);
  history_set_pos(found);
  gnash::readline::undo_clear();  // like history motion: not undoable
  rl_replace_line(e && e->line ? e->line : "", 1);
  rl_point = 0;
  return 0;
}

}  // namespace

extern "C" int rl_noninc_reverse_search(int /*count*/, int key) {
  if (!nsearch_read((key == '/' || key == '?') ? key : ':')) return rl_ding();
  return nsearch_do(-1);
}

extern "C" int rl_noninc_forward_search(int /*count*/, int key) {
  if (!nsearch_read((key == '/' || key == '?') ? key : ':')) return rl_ding();
  return nsearch_do(1);
}

extern "C" int rl_noninc_reverse_search_again(int /*count*/, int /*key*/) {
  return nsearch_do(-1);
}

extern "C" int rl_noninc_forward_search_again(int /*count*/, int /*key*/) {
  return nsearch_do(1);
}
