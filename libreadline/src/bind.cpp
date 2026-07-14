// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// bind.cpp -- key binding, the function map, and inputrc parsing.
//
// A working subset of bash 5.3 lib/readline/bind.c: rl_bind_key /
// rl_bind_keyseq (with "\C-x", "\M-f", "\e[A", "\xNN" sequence syntax), a
// name->function map (rl_named_function), rl_parse_and_bind for one inputrc
// line ("keyseq": function  or  set var value), and rl_read_init_file.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gnash/readline_internal.hpp"
#include "gnash/sh/xmalloc.hpp"
#include "readline/keymaps.h"
#include "readline/readline.h"
#include "readline/tilde.h"

// ---- function map ---------------------------------------------------------
namespace {

struct NamedFunc {
  const char *name;
  rl_command_func_t *func;
};

const NamedFunc kFunmap[] = {
    {"beginning-of-line", rl_beg_of_line},
    {"end-of-line", rl_end_of_line},
    {"forward-char", rl_forward_char},
    {"backward-char", rl_backward_char},
    {"forward-word", rl_forward_word},
    {"backward-word", rl_backward_word},
    {"delete-char", rl_delete},
    {"backward-delete-char", rl_rubout},
    {"kill-line", rl_kill_line},
    {"clear-screen", rl_clear_screen},
    {"backward-kill-line", rl_backward_kill_line},
    {"unix-line-discard", rl_unix_line_discard},
    {"kill-word", rl_kill_word},
    {"backward-kill-word", rl_backward_kill_word},
    {"unix-word-rubout", rl_unix_word_rubout},
    {"yank", rl_yank},
    {"yank-pop", rl_yank_pop},
    {"yank-nth-arg", rl_yank_nth_arg},
    {"yank-last-arg", rl_yank_last_arg},
    {"insert-last-argument", rl_yank_last_arg},
    {"abort", rl_abort},
    {"undo", rl_undo_command},
    {"revert-line", rl_revert_line},
    {"set-mark", rl_set_mark},
    {"exchange-point-and-mark", rl_exchange_point_and_mark},
    {"character-search", rl_char_search},
    {"character-search-backward", rl_backward_char_search},
    {"transpose-words", rl_transpose_words},
    {"delete-horizontal-space", rl_delete_horizontal_space},
    {"quoted-insert", rl_quoted_insert},
    {"tab-insert", rl_tab_insert},
    {"tilde-expand", rl_tilde_expand},
    {"insert-comment", rl_insert_comment},
    {"clear-display", rl_clear_display},
    {"do-lowercase-version", rl_do_lowercase_version},
    {"re-read-init-file", rl_re_read_init_file},
    {"execute-named-command", rl_execute_named_command},
    {"start-kbd-macro", rl_start_kbd_macro},
    {"end-kbd-macro", rl_end_kbd_macro},
    {"call-last-kbd-macro", rl_call_last_kbd_macro},
    {"beginning-of-history", rl_beginning_of_history},
    {"end-of-history", rl_end_of_history},
    {"fetch-history", rl_fetch_history},
    {"operate-and-get-next", rl_operate_and_get_next},
    {"non-incremental-forward-search-history", rl_noninc_forward_search},
    {"non-incremental-reverse-search-history", rl_noninc_reverse_search},
    {"menu-complete", rl_menu_complete},
    {"menu-complete-backward", rl_backward_menu_complete},
    {"vi-eof-maybe", rl_vi_eof_maybe},
    {"vi-match", rl_vi_match},
    {"vi-complete", rl_vi_complete},
    {"vi-tilde-expand", rl_vi_tilde_expand},
    {"vi-fetch-history", rl_vi_fetch_history},
    {"vi-search", rl_vi_search},
    {"vi-search-again", rl_vi_search_again},
    {"vi-replace", rl_vi_replace},
    {"vi-first-print", rl_vi_first_print},
    {"vi-yank-arg", rl_vi_yank_arg},
    {"vi-yank-to", rl_vi_yank_to},
    {"vi-column", rl_vi_column},
    {"vi-set-mark", rl_vi_set_mark},
    {"vi-goto-mark", rl_vi_goto_mark},
    {"vi-unix-word-rubout", rl_vi_unix_word_rubout},
    {"vi-undo", rl_vi_undo},
    {"vi-redo", rl_vi_redo},
    {"transpose-chars", rl_transpose_chars},
    {"upcase-word", rl_upcase_word},
    {"downcase-word", rl_downcase_word},
    {"capitalize-word", rl_capitalize_word},
    {"accept-line", rl_newline},
    {"previous-history", rl_get_previous_history},
    {"next-history", rl_get_next_history},
    {"complete", rl_complete},
    {"possible-completions", rl_possible_completions},
    {"insert-completions", rl_insert_completions},
    {"reverse-search-history", rl_reverse_search_history},
    {"forward-search-history", rl_forward_search_history},
    {"self-insert", rl_insert},
    {"digit-argument", rl_digit_argument},
    {"universal-argument", rl_universal_argument},
    {"vi-editing-mode", rl_vi_editing_mode},
    {"emacs-editing-mode", rl_emacs_editing_mode},
    {nullptr, nullptr},
};

// NULL-terminated, sorted list of bindable function names (for `bind -l').
extern "C" const char **rl_funmap_names(void) {
  static std::vector<const char *> names;
  if (names.empty()) {
    for (int i = 0; kFunmap[i].name; i++) names.push_back(kFunmap[i].name);
    std::sort(names.begin(), names.end(),
              [](const char *a, const char *b) { return std::strcmp(a, b) < 0; });
    names.push_back(nullptr);
  }
  return names.data();
}

int ctrl(int c) {
  if (c == '?') return 0x7f;
  return std::toupper(static_cast<unsigned char>(c)) & 0x1f;
}

// Translate a key-sequence string with backslash escapes into raw key codes.
std::vector<int> translate_keyseq(const char *s) {
  std::vector<int> out;
  while (*s) {
    if (*s != '\\') {
      out.push_back(static_cast<unsigned char>(*s++));
      continue;
    }
    s++;  // past backslash
    switch (*s) {
      case 'C':
      case 'c':
        if (s[1] == '-') {
          s += 2;
          if (*s) {
            out.push_back(ctrl(static_cast<unsigned char>(*s)));
            s++;
          }
        }
        break;
      case 'M':
      case 'm':
        if (s[1] == '-') {
          out.push_back(0x1b);  // meta -> ESC prefix
          s += 2;
        } else {
          s++;
        }
        break;
      case 'e':
        out.push_back(0x1b);
        s++;
        break;
      case 'n': out.push_back('\n'); s++; break;
      case 'r': out.push_back('\r'); s++; break;
      case 't': out.push_back('\t'); s++; break;
      case 'a': out.push_back('\a'); s++; break;
      case 'b': out.push_back('\b'); s++; break;
      case 'f': out.push_back('\f'); s++; break;
      case 'v': out.push_back('\v'); s++; break;
      case '\\': out.push_back('\\'); s++; break;
      case '"': out.push_back('"'); s++; break;
      case '\'': out.push_back('\''); s++; break;
      case 'x': {
        s++;
        int v = 0, n = 0;
        while (n < 2 && std::isxdigit(static_cast<unsigned char>(*s))) {
          int d = std::tolower(static_cast<unsigned char>(*s));
          v = v * 16 + (d <= '9' ? d - '0' : d - 'a' + 10);
          s++;
          n++;
        }
        out.push_back(v);
        break;
      }
      default:
        if (*s >= '0' && *s <= '7') {  // octal
          int v = 0, n = 0;
          while (n < 3 && *s >= '0' && *s <= '7') {
            v = v * 8 + (*s - '0');
            s++;
            n++;
          }
          out.push_back(v);
        } else if (*s) {
          out.push_back(static_cast<unsigned char>(*s++));
        }
        break;
    }
  }
  return out;
}

std::string trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
  return s.substr(a, b - a);
}

}  // namespace

extern "C" rl_command_func_t *rl_named_function(const char *name) {
  for (const NamedFunc *p = kFunmap; p->name; p++)
    if (std::strcmp(p->name, name) == 0) return p->func;
  return nullptr;
}

extern "C" int rl_bind_key(int key, rl_command_func_t *function) {
  if (key < 0 || key >= KEYMAP_SIZE) return -1;
  Keymap m = rl_get_keymap();
  m[key].type = ISFUNC;
  m[key].function = function;
  m[key].kmap = nullptr;
  return 0;
}

extern "C" int rl_bind_keyseq(const char *keyseq, rl_command_func_t *function) {
  std::vector<int> codes = translate_keyseq(keyseq);
  if (codes.empty()) return -1;

  Keymap m = rl_get_keymap();
  for (size_t i = 0; i + 1 < codes.size(); i++) {
    int c = codes[i];
    if (c < 0 || c >= KEYMAP_SIZE) return -1;
    if (m[c].type != ISKMAP || m[c].kmap == nullptr) {
      Keymap sub = rl_make_bare_keymap();
      m[c].type = ISKMAP;
      m[c].function = nullptr;
      m[c].kmap = sub;
    }
    m = static_cast<Keymap>(m[c].kmap);
  }
  int last = codes.back();
  if (last < 0 || last >= KEYMAP_SIZE) return -1;
  m[last].type = ISFUNC;
  m[last].function = function;
  m[last].kmap = nullptr;
  return 0;
}

extern "C" int rl_parse_and_bind(char *line) {
  if (line == nullptr) return 0;
  std::string s = trim(line);
  if (s.empty() || s[0] == '#') return 0;
  if (s[0] == '$') return 0;  // conditionals not yet supported

  // set var value
  if (s.compare(0, 4, "set ") == 0) {
    std::string rest = trim(s.substr(4));
    std::string var, val;
    size_t sp = rest.find_first_of(" \t");
    if (sp == std::string::npos) {
      var = rest;
    } else {
      var = rest.substr(0, sp);
      val = trim(rest.substr(sp + 1));
    }
    if (var == "editing-mode") {
      if (val == "vi")
        rl_vi_editing_mode(0, 0);
      else
        rl_emacs_editing_mode(0, 0);
    }
    // Other variables are accepted and ignored for now.
    return 0;
  }

  // keyseq: function
  size_t colon = s.find(':');
  if (colon == std::string::npos) return 0;
  std::string lhs = trim(s.substr(0, colon));
  std::string rhs = trim(s.substr(colon + 1));

  // Strip surrounding quotes from the key sequence.
  std::string seq = lhs;
  if (seq.size() >= 2 && seq.front() == '"' && seq.back() == '"')
    seq = seq.substr(1, seq.size() - 2);

  rl_command_func_t *func = rl_named_function(rhs.c_str());
  if (func == nullptr) return -1;

  return rl_bind_keyseq(seq.c_str(), func);
}

extern "C" int rl_read_init_file(const char *filename) {
  std::string path;
  if (filename)
    path = filename;
  else {
    const char *env = std::getenv("INPUTRC");
    if (env && *env)
      path = env;
    else {
      char *home = tilde_expand("~/.inputrc");
      path = home ? home : "";
      gnash::sh::xfree(home);
    }
  }
  if (path.empty()) return -1;

  FILE *f = std::fopen(path.c_str(), "r");
  if (f == nullptr) return -1;

  char buf[1024];
  while (std::fgets(buf, sizeof buf, f)) {
    size_t n = std::strlen(buf);
    if (n && buf[n - 1] == '\n') buf[n - 1] = '\0';
    rl_parse_and_bind(buf);
  }
  std::fclose(f);
  return 0;
}
