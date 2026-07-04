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

#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <pwd.h>
#include <string>
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
  while ((ent = readdir(dir)) != nullptr) {
    const char *name = ent->d_name;
    if (filename.empty()) {
      if (name[0] == '.' &&
          (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
        continue;  // skip . and .. when not explicitly requested
    } else if (std::strncmp(name, filename.c_str(), filename.size()) != 0) {
      continue;
    }
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
