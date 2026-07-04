// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// isearch.cpp -- incremental history search (C-r / C-s).
//
// A functional port of the interaction in bash 5.3 lib/readline/isearch.c:
// read keys, extend/trim the search string, walk the history for a line that
// contains it, and show it with the "(reverse-i-search)`str':" prompt.  RET
// accepts, C-g aborts (restoring the original line), C-r/C-s repeat, DEL trims.

#include <cstdio>
#include <cstring>
#include <string>

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
  std::fflush(o);
}

int isearch(int dir) {
  std::string orig(rl_line_buffer, static_cast<size_t>(rl_end));
  std::string ss;
  int matchpos = -1;  // history index currently displayed, -1 = none yet
  bool tty = rl_outstream != nullptr;

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
      // Any other key terminates the search, leaving the found line.
      return 0;
    }

    int f = search_history(ss, start, (dir < 0) ? -1 : 1);
    if (f >= 0) {
      matchpos = f;
      HIST_ENTRY *e = history_get(history_base + f);
      rl_replace_line(e->line, 1);
      rl_point = rl_end;
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
