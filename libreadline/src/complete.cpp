// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// complete.cpp -- the completion engine and its hook interface.
//
// Follows bash 5.3 lib/readline/complete.c in structure: find the word under
// the cursor, gather matches (first via the application's attempted-completion
// hook, else a per-match generator, else filename completion), compute the
// longest common prefix, insert it, and either append a character for a sole
// match or list the alternatives.  The shell plugs in programmable completion
// purely through rl_attempted_completion_function / rl_completion_entry_function
// and the rl_completer_* tunables -- libreadline has no shell knowledge.

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <pwd.h>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <vector>

#include "gnash/sh/xmalloc.hpp"
#include "readline/readline.h"
#include "readline/tilde.h"

using gnash::sh::savestring;
using gnash::sh::xfree;
using gnash::sh::xmalloc;

// ---- tunables -------------------------------------------------------------
extern "C" {
rl_completion_func_t *rl_attempted_completion_function = nullptr;
rl_compentry_func_t *rl_completion_entry_function = nullptr;
void (*rl_completion_display_matches_hook)(char **, int, int) = nullptr;

const char *rl_basic_word_break_characters = " \t\n\"\\'`@$><=;|&{(";
char *rl_completer_word_break_characters = nullptr;  // defaults to basic
const char *rl_basic_quote_characters = "\"'";
char *rl_completer_quote_characters = nullptr;
char *rl_filename_quote_characters = nullptr;

int rl_completion_append_character = ' ';
int rl_completion_suppress_append = 0;
int rl_completion_query_items = 100;
int rl_match_hidden_files = 1;  // zsh persona sets 0: no dotfiles without a `.'
int rl_ignore_completion_duplicates = 1;
int rl_completion_type = 0;
int rl_attempted_completion_over = 0;
int rl_filename_completion_desired = 0;
int rl_completion_found_quote = 0;
int rl_completion_mark_symlink_dirs = 0;
}

namespace {

const char *word_break_chars() {
  return rl_completer_word_break_characters ? rl_completer_word_break_characters
                                            : rl_basic_word_break_characters;
}

// Longest common prefix of two strings.
std::string common_prefix(const std::string &a, const std::string &b) {
  size_t n = 0;
  while (n < a.size() && n < b.size() && a[n] == b[n]) n++;
  return a.substr(0, n);
}

// Print matches in columns to rl_outstream, then repaint the line.
void display_matches(char **matches) {
  int count = 0;
  for (int i = 1; matches[i]; i++) count++;
  if (count == 0) return;

  if (rl_completion_display_matches_hook) {
    int longest = 0;
    for (int i = 1; matches[i]; i++) {
      int l = static_cast<int>(std::strlen(matches[i]));
      if (l > longest) longest = l;
    }
    rl_completion_display_matches_hook(matches, count, longest);
    return;
  }

  FILE *o = rl_outstream ? rl_outstream : stdout;
  int longest = 0;
  for (int i = 1; matches[i]; i++) {
    int l = static_cast<int>(std::strlen(matches[i]));
    if (l > longest) longest = l;
  }
  int colwidth = longest + 2;
  int screen = 80;
  int cols = colwidth > 0 ? screen / colwidth : 1;
  if (cols < 1) cols = 1;

  std::fputc('\n', o);
  int col = 0;
  for (int i = 1; matches[i]; i++) {
    std::fputs(matches[i], o);
    if (++col >= cols) {
      std::fputc('\n', o);
      col = 0;
    } else {
      int pad = colwidth - static_cast<int>(std::strlen(matches[i]));
      for (int p = 0; p < pad; p++) std::fputc(' ', o);
    }
  }
  if (col != 0) std::fputc('\n', o);
  std::fflush(o);
  rl_redisplay();
}

void free_matches(char **matches) {
  if (!matches) return;
  for (int i = 0; matches[i]; i++) xfree(matches[i]);
  xfree(matches);
}

char **gather_matches(const char *text, int start, int end) {
  char **matches = nullptr;
  rl_attempted_completion_over = 0;

  if (rl_attempted_completion_function)
    matches = (*rl_attempted_completion_function)(text, start, end);

  if (matches == nullptr && rl_attempted_completion_over == 0) {
    rl_compentry_func_t *gen = rl_completion_entry_function
                                   ? rl_completion_entry_function
                                   : rl_filename_completion_function;
    matches = rl_completion_matches(text, gen);
  }
  return matches;
}

int complete_internal(int what_to_do) {
  const char *brk = word_break_chars();
  int start = rl_point;
  while (start > 0 && std::strchr(brk, rl_line_buffer[start - 1]) == nullptr) start--;

  std::string text(rl_line_buffer + start, static_cast<size_t>(rl_point - start));
  char **matches = gather_matches(text.c_str(), start, rl_point);

  if (matches == nullptr || matches[0] == nullptr) {
    free_matches(matches);
    return rl_ding();
  }

  // matches[0] is the longest common prefix; matches[1..] are the real
  // matches.  Exactly one real match means matches[2] is NULL.
  bool unique = (matches[2] == nullptr);

  if (what_to_do == '?') {          // possible-completions: list only
    display_matches(matches);
    free_matches(matches);
    return 0;
  }

  // Replace the word with the longest common prefix.
  rl_delete_text(start, rl_point);
  rl_point = start;
  rl_insert_text(matches[0]);

  if (unique) {
    // For a filename completion that is a directory, append `/' rather than the
    // usual space so the next path component can be typed straight away.
    bool is_dir = false;
    if (rl_filename_completion_desired) {
      std::string path = matches[0];
      if (!path.empty() && path[0] == '~') {
        char *ex = tilde_expand(path.c_str());
        path = ex;
        xfree(ex);
      }
      struct stat st;
      if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) is_dir = true;
    }
    if (is_dir) {
      if (rl_point == 0 || rl_line_buffer[rl_point - 1] != '/') rl_insert_text("/");
    } else if (rl_completion_append_character && rl_completion_suppress_append == 0) {
      char a[2] = {static_cast<char>(rl_completion_append_character), '\0'};
      rl_insert_text(a);
    }
  } else if (std::strcmp(matches[0], text.c_str()) == 0) {
    // No progress and ambiguous: list the alternatives.
    display_matches(matches);
  }

  free_matches(matches);
  return 0;
}

// Lay `items` out in a grid at the terminal width, filled COLUMN-major (down
// each column, then across) as zsh does -- so a sorted list reads alphabetically
// down the columns.  Returns the number of screen rows; if `print` is set, also
// writes the grid to rl_outstream.  Used by the zsh menu both to list the
// candidates and to report the line count in the "see all N possibilities?" query.
int completion_grid(const std::vector<std::string> &items, bool print) {
  int n = static_cast<int>(items.size());
  int longest = 0;
  for (const auto &s : items)
    if (static_cast<int>(s.size()) > longest) longest = static_cast<int>(s.size());
  int colwidth = longest + 2;
  int width = rl_get_screen_width();
  if (width <= 0) width = 80;
  int cols = colwidth > 0 ? width / colwidth : 1;
  if (cols < 1) cols = 1;
  int rows = (n + cols - 1) / cols;
  if (print) {
    FILE *o = rl_outstream ? rl_outstream : stdout;
    std::fputc('\n', o);
    for (int r = 0; r < rows; r++) {
      for (int c = 0; c < cols; c++) {
        int idx = c * rows + r;  // column-major
        if (idx >= n) continue;
        std::fputs(items[idx].c_str(), o);
        if (c * rows + rows + r < n)  // not the last populated column in this row
          for (int p = colwidth - static_cast<int>(items[idx].size()); p > 0; p--)
            std::fputc(' ', o);
      }
      std::fputc('\n', o);
    }
    std::fflush(o);
  }
  return rows;
}

// ---- zsh-style menu completion --------------------------------------------
// The first TAB lists the candidates (sorted, case-insensitive) but does not
// change the line.  Each subsequent TAB inserts the next candidate in turn
// (Shift-TAB goes backward), cycling through the sorted list.  State persists
// between calls and is treated as a continuation only when the buffer is exactly
// what the previous step left -- same text and point -- so any edit restarts a
// fresh completion.
std::vector<std::string> menu_items;
int menu_idx = -1;  // -1 = list shown but nothing inserted yet
int menu_start = 0;

// Alphabetical order, ignoring case; ties broken by the case-sensitive order so
// the result is deterministic.
bool menu_less(const std::string &a, const std::string &b) {
  int c = strcasecmp(a.c_str(), b.c_str());
  return c < 0 || (c == 0 && a < b);
}

int menu_step(int dir) {
  // A continuation only if the previous command was also a menu step -- i.e. TAB
  // (or Shift-TAB) pressed again with no editing in between.  Any other action
  // starts a fresh completion, so stale state never leaks across lines.
  bool cont = (rl_last_func == rl_menu_complete ||
               rl_last_func == rl_backward_menu_complete) &&
              menu_items.size() > 1;
  if (!cont) {
    menu_items.clear();
    menu_idx = -1;
    const char *brk = word_break_chars();
    int start = rl_point;
    while (start > 0 && std::strchr(brk, rl_line_buffer[start - 1]) == nullptr) start--;
    std::string text(rl_line_buffer + start, static_cast<size_t>(rl_point - start));
    char **matches = gather_matches(text.c_str(), start, rl_point);
    if (matches == nullptr || matches[0] == nullptr) {
      free_matches(matches);
      return rl_ding();
    }
    // A sole match has nothing to cycle: complete it normally (append char/`/').
    if (matches[2] == nullptr) {
      free_matches(matches);
      return complete_internal('\t');
    }
    for (int i = 1; matches[i]; i++) menu_items.emplace_back(matches[i]);
    free_matches(matches);
    std::sort(menu_items.begin(), menu_items.end(), menu_less);
    menu_start = start;

    // The first TAB only lists the candidates; it does not touch the line.  A
    // small set is shown outright; a large one (> rl_completion_query_items)
    // first asks, like zsh.
    FILE *o = rl_outstream ? rl_outstream : stdout;
    bool show = true;
    if (rl_completion_query_items > 0 &&
        static_cast<int>(menu_items.size()) > rl_completion_query_items) {
      int rows = completion_grid(menu_items, /*print=*/false);
      std::fprintf(o, "\nzsh: do you wish to see all %zu possibilities (%d lines)? ",
                   menu_items.size(), rows);
      std::fflush(o);
      int c = rl_read_key();
      show = (c == 'y' || c == 'Y');
      if (!show) std::fputc('\n', o);  // leave the query line; drop to a new one
    }
    if (show) completion_grid(menu_items, /*print=*/true);
    return 0;  // the first TAB only lists; the line is left untouched
  }

  // Continuation: advance to the next candidate and put it on the line.
  int n = static_cast<int>(menu_items.size());
  if (menu_idx < 0)
    menu_idx = (dir < 0) ? n - 1 : 0;
  else
    menu_idx = (menu_idx + (dir < 0 ? n - 1 : 1)) % n;
  rl_delete_text(menu_start, rl_point);
  rl_point = menu_start;
  rl_insert_text(menu_items[static_cast<size_t>(menu_idx)].c_str());
  return 0;
}

}  // namespace

// ---- public API -----------------------------------------------------------

extern "C" char **rl_completion_matches(const char *text, rl_compentry_func_t *gen) {
  std::vector<char *> v;
  for (int state = 0;; state++) {
    char *m = (*gen)(text, state);
    if (m == nullptr) break;
    if (rl_ignore_completion_duplicates) {
      bool dup = false;
      for (char *e : v)
        if (std::strcmp(e, m) == 0) dup = true;
      if (dup) {
        xfree(m);
        continue;
      }
    }
    v.push_back(m);
  }
  if (v.empty()) return nullptr;

  std::string lcd = v[0];
  for (size_t i = 1; i < v.size(); i++) lcd = common_prefix(lcd, v[i]);

  char **ret = static_cast<char **>(xmalloc((v.size() + 2) * sizeof(char *)));
  ret[0] = savestring(lcd.c_str());
  for (size_t i = 0; i < v.size(); i++) ret[i + 1] = v[i];
  ret[v.size() + 1] = nullptr;
  return ret;
}

extern "C" char *rl_filename_completion_function(const char *text, int state) {
  static DIR *dir = nullptr;
  static std::string users_dirname;  // directory prefix as typed
  static std::string filename;       // basename prefix to match
  static std::string dirname;        // directory to actually read

  if (state == 0) {
    if (dir) {
      closedir(dir);
      dir = nullptr;
    }
    std::string t = text ? text : "";
    std::string::size_type slash = t.find_last_of('/');
    if (slash == std::string::npos) {
      users_dirname.clear();
      filename = t;
      dirname = ".";
    } else {
      users_dirname = t.substr(0, slash + 1);
      filename = t.substr(slash + 1);
      dirname = users_dirname;
    }
    std::string todir = dirname;
    if (!todir.empty() && todir[0] == '~') {
      char *ex = tilde_expand(todir.c_str());
      todir = ex;
      xfree(ex);
    }
    dir = opendir(todir.empty() ? "." : todir.c_str());
    rl_filename_completion_desired = 1;
  }

  if (dir == nullptr) return nullptr;

  struct dirent *ent;
  bool want_dot = !filename.empty() && filename[0] == '.';  // word begins with `.'
  while ((ent = readdir(dir)) != nullptr) {
    const char *name = ent->d_name;
    // `.' and `..' are never offered implicitly.
    if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
      continue;
    // A hidden file is offered only when the word itself begins with `.', or
    // when hidden-file matching is enabled (off in the zsh persona).
    if (name[0] == '.' && !want_dot && !rl_match_hidden_files) continue;
    if (!filename.empty() && std::strncmp(name, filename.c_str(), filename.size()) != 0)
      continue;
    std::string result = users_dirname + name;
    return savestring(result.c_str());
  }

  closedir(dir);
  dir = nullptr;
  return nullptr;
}

extern "C" char *rl_username_completion_function(const char *text, int state) {
  static std::string prefix;
  if (state == 0) {
    prefix = (text && text[0] == '~') ? std::string(text + 1) : std::string(text ? text : "");
    setpwent();
  }
  struct passwd *pw;
  while ((pw = getpwent()) != nullptr) {
    if (std::strncmp(pw->pw_name, prefix.c_str(), prefix.size()) == 0) {
      std::string r = "~" + std::string(pw->pw_name);
      return savestring(r.c_str());
    }
  }
  endpwent();
  return nullptr;
}

extern "C" int rl_complete(int /*count*/, int /*key*/) { return complete_internal('\t'); }
extern "C" int rl_possible_completions(int /*count*/, int /*key*/) {
  return complete_internal('?');
}
extern "C" int rl_insert_completions(int /*count*/, int /*key*/) {
  return complete_internal('*');
}
extern "C" int rl_menu_complete(int /*count*/, int /*key*/) { return menu_step(+1); }
extern "C" int rl_backward_menu_complete(int /*count*/, int /*key*/) { return menu_step(-1); }
