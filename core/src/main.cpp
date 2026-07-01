// main.cpp -- the gnash executable.
//
//   gnash                 # interactive REPL if stdin is a terminal, else run stdin
//   gnash -c COMMAND [name [args...]]
//   gnash SCRIPT [args...]
//   gnash -l / --login    # login shell (also if argv[0] begins with `-')
//
// Startup files follow bash, with the names derived from the invocation name
// (the basename of argv[0], minus a leading `-').  Invoked as "gnash" it reads
// ~/.gnash_profile / ~/.gnashrc and $GNASH_ENV; invoked as "bash" it reads
// ~/.bash_profile / ~/.bashrc and $BASH_ENV; system-wide files are shared.
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "gnash/core/shell.hpp"

namespace {

bool file_readable(const std::string &path) { return access(path.c_str(), R_OK) == 0; }

// Run FILE in the current shell if it exists (the `source' semantics used for
// every startup file).
void source_if_exists(gnash::core::Shell &sh, const std::string &path) {
  if (!file_readable(path)) return;
  std::ifstream f(path);
  if (!f) return;
  std::ostringstream ss;
  ss << f.rdbuf();
  sh.run_string(ss.str());
}

std::string upper(std::string s) {
  for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// Read the startup files appropriate to this shell's mode, mirroring bash but
// with per-invocation names (prefix == "gnash", "bash", ...).
void read_startup_files(gnash::core::Shell &sh, const std::string &prefix, bool login,
                        bool interactive) {
  const char *home = std::getenv("HOME");
  std::string h = home ? home : "";

  if (login) {
    // System profile, then the first personal profile that exists.
    source_if_exists(sh, "/etc/profile");
    if (!h.empty()) {
      const std::string candidates[] = {h + "/." + prefix + "_profile",
                                        h + "/." + prefix + "_login", h + "/.profile"};
      for (const std::string &c : candidates)
        if (file_readable(c)) { source_if_exists(sh, c); break; }
    }
  } else if (interactive) {
    // System bashrc analogue, then the personal rc.
    source_if_exists(sh, "/etc/" + prefix + "." + prefix + "rc");
    if (!h.empty()) source_if_exists(sh, h + "/." + prefix + "rc");
  } else {
    // Non-interactive: the file named by <PREFIX>_ENV, if any.
    const char *env = std::getenv((upper(prefix) + "_ENV").c_str());
    if (env && *env) source_if_exists(sh, env);
  }
}

}  // namespace

int main(int argc, char **argv) {
  gnash::core::Shell sh;

  // The invocation name: basename of argv[0], minus a leading `-' (which marks
  // a login shell).  It drives both error-message prefixes and startup-file
  // names.
  std::string prog = argc > 0 ? argv[0] : "gnash";
  bool login = !prog.empty() && prog[0] == '-';
  std::string base = login ? prog.substr(1) : prog;
  auto slash = base.rfind('/');
  if (slash != std::string::npos) base = base.substr(slash + 1);
  if (base.empty()) base = "gnash";
  sh.shell_name = base;
  const std::string prefix = base;

  // Collect args, honoring leading -l/--login and -i before the command/script.
  std::vector<std::string> args(argv + 1, argv + argc);
  bool force_interactive = false;
  while (!args.empty()) {
    if (args[0] == "-l" || args[0] == "--login") login = true;
    else if (args[0] == "-i") force_interactive = true;
    else break;
    args.erase(args.begin());
  }

  if (!args.empty() && args[0] == "-c") {
    sh.init_job_control(false);
    read_startup_files(sh, prefix, login, false);
    std::string cmd = args.size() > 1 ? args[1] : "";
    if (args.size() > 2) {
      sh.arg0 = args[2];
      sh.positional.assign(args.begin() + 3, args.end());
    }
    sh.run_string(cmd);
  } else if (!args.empty()) {
    sh.init_job_control(false);
    std::ifstream f(args[0]);
    if (!f) {
      std::fprintf(stderr, "%s: %s: cannot open\n", sh.shell_name.c_str(), args[0].c_str());
      return 127;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    sh.arg0 = args[0];
    sh.shell_name = args[0];  // scripts report errors as "SCRIPT: line N: ..."
    sh.positional.assign(args.begin() + 1, args.end());
    read_startup_files(sh, prefix, login, false);
    sh.run_string(ss.str());
  } else if (force_interactive || isatty(STDIN_FILENO)) {
    sh.interactive = true;
    sh.init_job_control(true);
    read_startup_files(sh, prefix, login, true);
    return gnash::core::run_interactive(sh);
  } else {
    sh.init_job_control(false);
    read_startup_files(sh, prefix, login, false);
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    sh.run_string(ss.str());
  }

  int rc = sh.exiting ? sh.exit_status : sh.last_status;

  // Run the EXIT trap, if any, before terminating.
  auto it = sh.traps.find("EXIT");
  if (it != sh.traps.end()) {
    std::string cmd = it->second;
    sh.traps.erase(it);
    sh.exiting = false;
    sh.run_string(cmd);
  }

  // A login shell reads ~/.<prefix>_logout as it exits.
  if (login) {
    const char *home = std::getenv("HOME");
    if (home) source_if_exists(sh, std::string(home) + "/." + prefix + "_logout");
  }
  return rc;
}
