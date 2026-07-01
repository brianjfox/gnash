// repl.cpp -- the interactive read-eval-print loop.
//
// This is where the pieces meet: libreadline provides line editing, libhistory
// provides history + `!'-expansion, and the shell core parses and executes.
// Multi-line constructs (unterminated quotes, `if'/`for' without their closers,
// here-documents, trailing `\') are continued with the PS2 prompt.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "gnash/core/parser.hpp"
#include "gnash/core/shell.hpp"
#include "readline/history.h"
#include "readline/readline.h"

namespace gnash::core {

namespace {
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
}  // namespace

int run_interactive(Shell &sh) {
  sh.init_job_control(true);
  sh.interactive = true;

  std::string histfile = history_path();
  if (!histfile.empty()) read_history(histfile.c_str());
  using_history();

  // Report background-job completion asynchronously while idle at the prompt.
  g_notify_shell = &sh;
  rl_event_hook = gnash_job_notify_hook;

  while (!sh.exiting) {
    sh.reap_jobs(true);      // report any finished background jobs
    sh.run_pending_traps();  // deliver signals received while at the prompt

    std::string ps1 = sh.is_set("PS1") ? sh.get("PS1") : std::string("\\s-\\v\\$ ");
    char *line = readline(expand_prompt(sh, ps1).c_str());
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
      if (!cont) break;  // EOF mid-command: run what we have
      if (!backslash) input += "\n";
      input += cont;
      std::free(cont);
    }

    if (input.find_first_not_of(" \t\n") != std::string::npos) add_history(input.c_str());
    sh.run_string(input);
  }

  rl_event_hook = nullptr;
  g_notify_shell = nullptr;

  if (!histfile.empty()) write_history(histfile.c_str());
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
