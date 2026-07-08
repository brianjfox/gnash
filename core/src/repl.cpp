// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// repl.cpp -- the interactive read-eval-print loop.
//
// This is where the pieces meet: libreadline provides line editing, libhistory
// provides history + `!'-expansion, and the shell core parses and executes.
// Multi-line constructs (unterminated quotes, `if'/`for' without their closers,
// here-documents, trailing `\') are continued with the PS2 prompt.

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "gnash/core/builtins.hpp"
#include "gnash/core/parser.hpp"
#include "gnash/core/shell.hpp"
#include "readline/history.h"
#include "readline/keymaps.h"
#include "readline/readline.h"

extern "C" void strmatch_set_interrupt(int);

namespace gnash::core {

namespace {
// SIGINT while a command is executing (interactive): record it and nudge the
// pattern matcher to bail.  The executor's unwinding() check aborts the running
// command so the loop reprompts instead of the shell being killed.  Both stores
// are async-signal-safe.
void interactive_sigint_handler(int) {
  g_sigint_received = 1;
  strmatch_set_interrupt(1);
}

bool trailing_backslash(const std::string &s) {
  int n = 0;
  for (auto it = s.rbegin(); it != s.rend() && *it == '\\'; ++it) n++;
  return (n % 2) == 1;
}

std::string history_path() {
  const char *h = std::getenv("HISTFILE");
  if (h && *h) return h;
  const char *home = std::getenv("HOME");
  return home ? std::string(home) + "/.gnash_history" : std::string();
}

// Registered as readline's idle hook so background-job completion is reported
// the moment it happens, rather than waiting for the next line of input.  When
// there is something to report we erase the current input line, print the
// notice above it, and redraw the prompt with whatever the user had typed.
Shell *g_notify_shell = nullptr;
extern "C" int gnash_job_notify_hook(void) {
  if (g_notify_shell && g_notify_shell->check_job_events()) {
    rl_clear_current_line();
    g_notify_shell->emit_job_notices();
    rl_redisplay();
  }
  return 0;
}

// Generate defined-variable names matching the partial name TEXT, one per call.
extern "C" char *gnash_var_completion(const char *text, int state) {
  static std::vector<std::string> names;
  static size_t idx;
  if (state == 0) {
    names.clear();
    idx = 0;
    std::string pref = text ? text : "";
    std::set<std::string> seen;
    if (g_notify_shell) {
      for (const auto &kv : g_notify_shell->vars)
        if (kv.first.compare(0, pref.size(), pref) == 0 && seen.insert(kv.first).second)
          names.push_back(kv.first);
      // Dynamic specials ($RANDOM, $SECONDS, ...) aren't in the variable table.
      for (const std::string &s : Shell::special_var_names())
        if (s.compare(0, pref.size(), pref) == 0 && seen.insert(s).second) names.push_back(s);
    }
    std::sort(names.begin(), names.end());
  }
  if (idx >= names.size()) return nullptr;
  return strdup(names[idx++].c_str());  // freed with free() by readline's xfree
}

// zsh-style syntax highlighting: color the command word green if it would run,
// red if not; quoted strings yellow.  Fills a color id per character
// (0=none, 1=green, 2=red, 3=yellow).
static bool is_assignment_word(const std::string &w) {
  size_t i = 0;
  if (i >= w.size() || !(std::isalpha((unsigned char)w[i]) || w[i] == '_')) return false;
  while (i < w.size() && (std::isalnum((unsigned char)w[i]) || w[i] == '_')) i++;
  return i < w.size() && w[i] == '=';
}

extern "C" void gnash_zsh_highlight(const char *line, int len, int *colors) {
  for (int i = 0; i < len; i++) colors[i] = 0;
  if (!g_notify_shell) return;
  bool cmd_pos = true;
  int i = 0;
  while (i < len) {
    char c = line[i];
    if (c == ' ' || c == '\t') { i++; continue; }
    if (c == ';' || c == '|' || c == '&' || c == '(' || c == '{' || c == '\n') {
      cmd_pos = true; i++; continue;
    }
    if (c == '#') { i = len; break; }  // comment to end of line
    if (c == '\'' || c == '"') {       // quoted string -> yellow
      char q = c; int start = i++;
      while (i < len && line[i] != q) { if (q == '"' && line[i] == '\\' && i + 1 < len) i++; i++; }
      if (i < len) i++;
      for (int k = start; k < i; k++) colors[k] = 3;
      cmd_pos = false;
      continue;
    }
    int start = i;
    while (i < len && std::strchr(" \t;|&(){}<>'\"\n#", line[i]) == nullptr) i++;
    if (i == start) {
      // A break character the cases above don't consume (`)', `}', `<', `>').
      // Skip it so the scan always advances -- otherwise a line containing one
      // (e.g. completing `Movie (2005)') spins here forever, hanging redisplay.
      i++;
      continue;
    }
    std::string word(line + start, static_cast<size_t>(i - start));
    if (cmd_pos) {
      if (is_assignment_word(word)) continue;  // VAR=val prefix: stays in cmd pos
      int color = command_is_valid(*g_notify_shell, word) ? 1 : 2;
      for (int k = start; k < i; k++) colors[k] = color;
      cmd_pos = false;
    }
  }
}

// True when the word starting at `start' is in command position: the first word
// of a simple command.  That is the start of the line, or just after a command
// separator (`;' `|' `&' `(' `{' backtick newline), skipping any leading
// `VAR=val' assignment prefixes (which keep the next word in command position).
static bool at_command_position(int start) {
  int p = start - 1;
  for (;;) {
    while (p >= 0 && (rl_line_buffer[p] == ' ' || rl_line_buffer[p] == '\t')) p--;
    if (p < 0) return true;
    char c = rl_line_buffer[p];
    if (c == ';' || c == '|' || c == '&' || c == '(' || c == '{' || c == '`' || c == '\n')
      return true;
    int e = p;  // end of the preceding token
    while (p >= 0 && rl_line_buffer[p] != ' ' && rl_line_buffer[p] != '\t' &&
           std::strchr(";|&(){}`\n", rl_line_buffer[p]) == nullptr)
      p--;
    std::string tok(rl_line_buffer + p + 1, static_cast<size_t>(e - p));
    if (!is_assignment_word(tok)) return false;  // a real command word precedes us
    // else: an assignment prefix -- keep scanning, we are still in command position
  }
}

// readline generator: successive command names matching `text' (built once when
// state == 0), for command-position completion.
extern "C" char *gnash_command_completion(const char *text, int state) {
  static std::vector<std::string> matches;
  static size_t idx;
  if (state == 0) {
    matches = g_notify_shell ? command_completions(*g_notify_shell, text)
                             : std::vector<std::string>();
    idx = 0;
  }
  if (idx < matches.size()) return strdup(matches[idx++].c_str());
  return nullptr;
}

// readline's attempted-completion hook.  A word introduced by `$'/`${' completes
// over shell variables; a word in command position (with no `/') completes over
// all command names (keywords/aliases/functions/builtins/$PATH); otherwise fall
// back to readline's default filename completion.
extern "C" char **gnash_attempted_completion(const char *text, int start, int end) {
  (void)end;
  bool dollar = start > 0 && rl_line_buffer[start - 1] == '$';
  bool braced = start > 1 && rl_line_buffer[start - 1] == '{' && rl_line_buffer[start - 2] == '$';
  if (dollar || braced) {
    rl_attempted_completion_over = 1;  // don't fall back to filename completion
    return rl_completion_matches(text, gnash_var_completion);
  }
  // Command position: complete command names, unless the word is a path (has a
  // `/'), in which case a filename completion of the executable is wanted.
  if (std::strchr(text, '/') == nullptr && at_command_position(start)) {
    rl_attempted_completion_over = 1;
    return rl_completion_matches(text, gnash_command_completion);
  }
  return nullptr;  // let readline fall back to its default (filename) completion
}

// display-shell-version (bound to C-x C-v, as in bash): print a line describing
// this shell -- gnash's own version and architecture, plus the personality it is
// emulating and that shell's version -- then redraw the prompt and input line.
extern "C" int gnash_display_shell_version(int /*count*/, int /*key*/) {
  FILE *o = rl_outstream ? rl_outstream : stdout;
  std::string line = "gnash, version " GNASH_VERSION;
  if (g_notify_shell) {
    Shell &sh = *g_notify_shell;
    std::string mach = sh.get("MACHTYPE");
    std::string pname = sh.get("GNASH_PERSONALITY");
    std::string pver;
    switch (sh.persona) {
      case Shell::Persona::Zsh: pver = sh.get("ZSH_VERSION"); break;
      case Shell::Persona::Ksh: pver = sh.get("KSH_VERSION"); break;
      case Shell::Persona::Csh: pver = sh.get("version"); break;
      case Shell::Persona::Ash: break;  // ash advertises no version
      default: pver = sh.get("BASH_VERSION"); break;  // Bash / gnash
    }
    line = "gnash, version " GNASH_VERSION " (" + mach + ")";
    if (!pname.empty()) {
      line += ", personality " + pname;
      if (!pver.empty()) line += " " + pver;
    }
  }
  std::fputs("\r\n", o);
  std::fprintf(o, "%s\n", line.c_str());
  std::fflush(o);
  rl_redisplay();  // repaint the prompt and current input below the version line
  return 0;
}

// (Re)apply the persona-dependent readline configuration.  The zsh persona gets
// live command-line highlighting, TAB menu-completion (Shift-TAB backward), and
// dotfile hiding; the other personas use readline's defaults -- no highlighting,
// TAB inserts the common prefix then lists.  Called at startup and again
// whenever the personality changes at runtime, so `personality zsh' / `emulate'
// take effect immediately.  The completion keys are bound in both the emacs and
// the vi-insert keymaps, so the persona's style applies in either editing mode;
// the read loop selects the mode's keymap per line.
void apply_persona_readline(Shell &sh) {
  bool zsh = sh.is_zsh();
  rl_highlight_function = zsh ? gnash_zsh_highlight : nullptr;
  rl_match_hidden_files = zsh ? 0 : 1;
  rl_command_func_t *tab = zsh ? rl_menu_complete : rl_complete;
  rl_command_func_t *shift_tab = zsh ? rl_backward_menu_complete : rl_complete;
  Keymap saved = rl_get_keymap();
  for (Keymap km : {rl_get_keymap_by_name("emacs"), vi_insertion_keymap}) {
    if (!km) continue;
    rl_set_keymap(km);
    rl_bind_key('\t', tab);
    rl_bind_keyseq("\\e[Z", shift_tab);
  }
  rl_set_keymap(saved);
}
}  // namespace

int run_interactive(Shell &sh) {
  sh.init_job_control(true);
  sh.interactive = true;

  // The history file is $HISTFILE (set per-personality in main, e.g. the bash
  // persona defaults to ~/.bash_history), falling back to the built-in default.
  std::string histfile = sh.get("HISTFILE");
  if (histfile.empty()) histfile = history_path();
  if (!histfile.empty()) read_history(histfile.c_str());
  using_history();
  // Keep at most $HISTSIZE entries in memory (bash default 500).
  int histsize = std::atoi(sh.get("HISTSIZE").c_str());
  if (histsize > 0) stifle_history(histsize);

  // Report background-job completion asynchronously while idle at the prompt.
  g_notify_shell = &sh;
  rl_event_hook = gnash_job_notify_hook;
  // Complete variable names after `$'.
  rl_attempted_completion_function = gnash_attempted_completion;

  // Bind C-x C-v to display-shell-version, as bash does by default.  Build the
  // keymaps first (idempotent) and target the emacs keymap so the sequence
  // descends into the existing C-x prefix map.
  rl_initialize();
  rl_set_keymap(rl_get_keymap_by_name("emacs"));
  rl_bind_keyseq("\\C-x\\C-v", gnash_display_shell_version);

  // Persona-dependent readline hooks (highlighting, TAB completion style, dotfile
  // hiding).  Applied now, and re-applied whenever the personality is switched at
  // runtime via `personality'/`emulate'.
  apply_persona_readline(sh);
  sh.on_personality_change = [&sh]() { apply_persona_readline(sh); };

  while (!sh.exiting) {
    // Catch SIGINT during command execution so C-c aborts the running command
    // and reprompts, rather than killing the shell.  (readline saves/restores
    // this around its own handler while reading a line.)  Re-asserted each
    // iteration, but never over a user INT trap -- so `trap ... INT' keeps
    // working and `trap - INT' cleanly restores this default next prompt.
    if (sh.traps.find("INT") == sh.traps.end()) {
      struct sigaction sa;
      std::memset(&sa, 0, sizeof sa);
      sigemptyset(&sa.sa_mask);
      sa.sa_flags = SA_RESTART;
      sa.sa_handler = interactive_sigint_handler;
      sigaction(SIGINT, &sa, nullptr);
    }

    sh.reap_jobs(true);      // report any finished background jobs
    sh.run_pending_traps();  // deliver signals received while at the prompt

    std::string ps1 = sh.is_set("PS1") ? sh.get("PS1") : std::string("\\u@\\h:\\w\\$ ");
    char *line = readline(expand_prompt(sh, ps1).c_str());
    if (rl_pending_sigint) {  // C-c: discard the line and reprompt fresh
      std::free(line);
      continue;
    }
    if (!line) {  // Ctrl-D on an empty line
      std::printf("exit\n");
      break;
    }
    std::string input(line);
    std::free(line);

    // History (`!!', `!n', `^old^new^', ...) expansion.
    char *expanded = nullptr;
    int hr = history_expand(const_cast<char *>(input.c_str()), &expanded);
    if (expanded) {
      if (hr < 0) {
        std::fprintf(stderr, "%s\n", expanded);
        std::free(expanded);
        continue;
      }
      input = expanded;
      std::free(expanded);
      if (hr == 2) {  // `:p' modifier -- print, don't execute
        std::printf("%s\n", input.c_str());
        continue;
      }
    }

    // Continue reading while the command is incomplete.
    bool interrupted = false;
    for (;;) {
      bool backslash = trailing_backslash(input);
      bool need_more;
      if (backslash) {
        input.pop_back();  // drop the line-continuation backslash
        need_more = true;
      } else {
        need_more = parse(input).incomplete;
      }
      if (!need_more) break;
      std::string ps2 = sh.is_set("PS2") ? sh.get("PS2") : std::string("> ");
      char *cont = readline(expand_prompt(sh, ps2).c_str());
      if (rl_pending_sigint) {  // C-c mid-command: abandon the whole thing
        std::free(cont);
        interrupted = true;
        break;
      }
      if (!cont) break;  // EOF mid-command: run what we have
      if (!backslash) input += "\n";
      input += cont;
      std::free(cont);
    }
    if (interrupted) continue;  // reprompt with PS1, discarding the partial command

    if (input.find_first_not_of(" \t\n") != std::string::npos) add_history(input.c_str());
    g_sigint_received = 0;  // start the command uninterrupted
    strmatch_set_interrupt(0);
    sh.run_string(input);
    if (g_sigint_received) {  // C-c aborted the command mid-run: reprompt cleanly
      g_sigint_received = 0;
      strmatch_set_interrupt(0);
      sh.last_status = 130;  // 128 + SIGINT, as bash reports
      std::fputc('\n', stderr);
    }
    sh.command_number++;  // \# advances for the next prompt
  }

  rl_event_hook = nullptr;
  g_notify_shell = nullptr;

  // Save to the current $HISTFILE (a startup file may have changed it) and
  // truncate it to $HISTFILESIZE lines, as bash does on exit.
  std::string savefile = sh.get("HISTFILE");
  if (savefile.empty()) savefile = histfile;
  if (!savefile.empty()) {
    write_history(savefile.c_str());
    int hfs = std::atoi(sh.get("HISTFILESIZE").c_str());
    if (hfs > 0) history_truncate_file(savefile.c_str(), hfs);
  }
  int rc = sh.exiting ? sh.exit_status : sh.last_status;

  // EXIT trap.
  auto it = sh.traps.find("EXIT");
  if (it != sh.traps.end()) {
    std::string cmd = it->second;
    sh.traps.erase(it);
    sh.exiting = false;
    sh.run_string(cmd);
  }
  return rc;
}

}  // namespace gnash::core
