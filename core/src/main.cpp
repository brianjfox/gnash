// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// main.cpp -- the gnash executable and its command-line handling.
//
//   gnash [options] [script [args...]]
//   gnash [options] -c COMMAND [name [args...]]
//
// Option parsing follows bash: single-letter flags may be grouped (-lx), a
// leading '+' unsets a flag, long options start with '--', and '-'/'--' end
// option processing.  Startup files follow bash too, with the personal-file
// names derived from the invocation name (basename of argv[0], minus a leading
// '-'): invoked as "gnash" it reads ~/.gnash_profile / ~/.gnashrc / $GNASH_ENV;
// invoked as "bash" it reads ~/.bash_profile / ~/.bashrc / $BASH_ENV.  As a
// convenience the gnash persona falls back to the bash-named files when its own
// are absent, so gnash works out of the box for users with only ~/.bash* files.
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <sys/utsname.h>
#include <unistd.h>
#include <vector>

#include "gnash/core/builtins.hpp"
#include "gnash/core/expand.hpp"
#include "gnash/core/shell.hpp"

namespace {

using gnash::core::Expander;
using gnash::core::Shell;

bool file_readable(const std::string &path) { return access(path.c_str(), R_OK) == 0; }

void source_if_exists(Shell &sh, const std::string &path) {
  if (!file_readable(path)) return;
  std::ifstream f(path);
  if (!f) return;
  std::ostringstream ss;
  ss << f.rdbuf();
  sh.run_string(ss.str());
  // A top-level `return' ends the startup file (as when sourced), it must not
  // unwind into the next startup file or the main command (`exit' still exits).
  sh.returning = false;
}

std::string upper(std::string s) {
  for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// Make PATH absolute against the current directory and resolve `.'/`..'
// lexically (no symlink resolution), as bash does for $BASH.
std::string canon_abs(std::string path) {
  if (path.empty() || path[0] != '/') {
    char cwd[4096];
    if (getcwd(cwd, sizeof cwd)) path = std::string(cwd) + "/" + path;
  }
  std::vector<std::string> comps;
  size_t i = 0;
  while (i < path.size()) {
    while (i < path.size() && path[i] == '/') i++;
    size_t j = i;
    while (j < path.size() && path[j] != '/') j++;
    std::string c = path.substr(i, j - i);
    i = j;
    if (c.empty() || c == ".") continue;
    if (c == "..") { if (!comps.empty()) comps.pop_back(); continue; }
    comps.push_back(c);
  }
  std::string r;
  for (const std::string &c : comps) { r += '/'; r += c; }
  return r.empty() ? "/" : r;
}

// The full pathname used to execute this shell, for $BASH: argv[0] made
// absolute, or found on $PATH when it is a bare name.
std::string resolve_exec_path(std::string a0) {
  if (!a0.empty() && a0[0] == '-') a0 = a0.substr(1);  // login-shell marker
  if (a0.empty()) return a0;
  if (a0.find('/') != std::string::npos) return canon_abs(a0);
  const char *path = std::getenv("PATH");
  std::string p = path ? path : "";
  size_t start = 0;
  for (;;) {
    size_t end = p.find(':', start);
    std::string dir = p.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (dir.empty()) dir = ".";
    std::string full = dir + "/" + a0;
    if (access(full.c_str(), X_OK) == 0) return canon_abs(full);
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return a0;  // not found on PATH; fall back to the bare name
}

// Options that carry startup-file policy.
struct StartupOpts {
  bool norc = false;
  bool noprofile = false;
  std::string rcfile;  // --rcfile / --init-file override for the personal rc
};

// Read the startup files appropriate to this shell's mode, mirroring bash but
// with per-invocation names (prefix == "gnash", "bash", ...).
// zsh reads .zshenv always, .zprofile/.zlogin for login shells, and .zshrc
// for interactive shells (system copies under /etc first).
void read_zsh_startup_files(Shell &sh, bool login, bool interactive, const StartupOpts &o) {
  const char *home = std::getenv("HOME");
  std::string h = home ? home : "";
  auto both = [&](const std::string &sys, const std::string &personal) {
    source_if_exists(sh, sys);
    if (!h.empty()) source_if_exists(sh, h + "/" + personal);
  };
  if (!o.norc) both("/etc/zshenv", ".zshenv");
  if (login && !o.noprofile) both("/etc/zprofile", ".zprofile");
  if (interactive && !o.norc) {
    if (!o.rcfile.empty()) source_if_exists(sh, o.rcfile);
    else both("/etc/zshrc", ".zshrc");
  }
  if (login && !o.noprofile) both("/etc/zlogin", ".zlogin");
}

// POSIX shells (ash/dash/ksh): a login shell reads /etc/profile and ~/.profile;
// an interactive shell reads the file named by $ENV (after parameter expansion).
void read_posix_startup_files(Shell &sh, bool login, bool interactive, const StartupOpts &o) {
  const char *home = std::getenv("HOME");
  std::string h = home ? home : "";
  if (login && !o.noprofile) {
    source_if_exists(sh, "/etc/profile");
    if (!h.empty()) source_if_exists(sh, h + "/.profile");
  }
  if (interactive && !o.norc) {
    std::string envfile = o.rcfile;
    if (envfile.empty()) { const char *e = std::getenv("ENV"); if (e) envfile = e; }
    if (!envfile.empty()) source_if_exists(sh, Expander(sh).expand_no_split(envfile));
  }
}

// csh/tcsh: a login shell reads /etc/csh.cshrc then /etc/csh.login then
// ~/.cshrc (or ~/.tcshrc) then ~/.login; a non-login interactive shell reads
// just the cshrc files.  These files are csh syntax, so source_if_exists runs
// them through the csh interpreter (run_string dispatches on the persona).
void read_csh_startup_files(Shell &sh, bool login, bool interactive, const StartupOpts &o) {
  const char *home = std::getenv("HOME");
  std::string h = home ? home : "";
  if (!o.norc) {
    source_if_exists(sh, "/etc/csh.cshrc");
    if (!h.empty()) {
      if (file_readable(h + "/.tcshrc")) source_if_exists(sh, h + "/.tcshrc");
      else source_if_exists(sh, h + "/.cshrc");
    }
  }
  if (login && !o.noprofile) {
    source_if_exists(sh, "/etc/csh.login");
    if (!h.empty()) source_if_exists(sh, h + "/.login");
  }
  (void)interactive;
}

void read_startup_files(Shell &sh, const std::string &prefix, bool login, bool interactive,
                        const StartupOpts &o) {
  if (sh.is_zsh()) { read_zsh_startup_files(sh, login, interactive, o); return; }
  if (sh.is_csh()) { read_csh_startup_files(sh, login, interactive, o); return; }
  if (sh.is_ash() || sh.is_ksh()) { read_posix_startup_files(sh, login, interactive, o); return; }

  const char *home = std::getenv("HOME");
  std::string h = home ? home : "";

  // The gnash persona falls back to bash's dotfiles: if no ~/.gnash* startup
  // file exists, gnash reads the bash analogue (~/.bash_profile, ~/.bashrc,
  // ...), so gnash behaves like bash for users who never wrote gnash rc files.
  // `prefixes' is the ordered list of names to try; for every other personality
  // it holds just its own name.
  std::vector<std::string> prefixes = {prefix};
  if (prefix == "gnash") prefixes.push_back("bash");

  if (login) {
    if (!o.noprofile) {
      source_if_exists(sh, "/etc/profile");
      if (!h.empty()) {
        std::vector<std::string> candidates;
        for (const std::string &p : prefixes) {
          candidates.push_back(h + "/." + p + "_profile");
          candidates.push_back(h + "/." + p + "_login");
        }
        candidates.push_back(h + "/.profile");
        for (const std::string &c : candidates)
          if (file_readable(c)) { source_if_exists(sh, c); break; }
      }
    }
  } else if (interactive) {
    if (!o.norc) {
      if (!o.rcfile.empty()) {
        source_if_exists(sh, o.rcfile);
      } else {
        for (const std::string &p : prefixes)
          source_if_exists(sh, "/etc/" + p + "." + p + "rc");
        if (!h.empty())
          for (const std::string &p : prefixes)
            if (file_readable(h + "/." + p + "rc")) {
              source_if_exists(sh, h + "/." + p + "rc");
              break;
            }
      }
    }
  } else {
    const char *env = nullptr;
    for (const std::string &p : prefixes)
      if ((env = std::getenv((upper(p) + "_ENV").c_str())) && *env) break;
    if (env && *env) source_if_exists(sh, env);
  }
}

// Apply a `set'-style long option name (from -o NAME).
void apply_set_o(Shell &sh, const std::string &name, bool set) {
  if (name == "errexit") sh.opt_errexit = set;
  else if (name == "xtrace") sh.opt_xtrace = set;
  else if (name == "nounset") sh.opt_nounset = set;
  else if (name == "noglob") sh.opt_noglob = set;
  else if (name == "verbose") sh.opt_verbose = set;
  else if (name == "noexec") sh.opt_noexec = set;
  // Other -o names (pipefail, posix, vi, emacs, ...) are accepted and ignored.
}

// Configure the shell's personality (which other shell it behaves as) and the
// identity variables that differ between shells.
void configure_persona(Shell &sh, const std::string &personality, const std::string &exec_path) {
  (void)exec_path;  // read back from $SHELL inside Shell::set_personality
  sh.set("GNASH_VERSION", GNASH_VERSION);  // gnash's own version, regardless of persona

  std::string mach = "unknown";
  struct utsname u;
  if (uname(&u) == 0) {
    std::string sys = u.sysname;
    for (char &c : sys) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string vendor = (sys == "darwin") ? "apple" : "unknown";
    mach = std::string(u.machine) + "-" + vendor + "-" + sys + u.release;
  }
  sh.set("MACHTYPE", mach);

  // The persona enum + per-shell identity variables; shared with the runtime
  // `personality'/`emulate' builtin.
  sh.set_personality(personality);
}

}  // namespace

int main(int argc, char **argv) {
  Shell sh;
  shopt_seed(sh);  // seed default shopt states so $BASHOPTS is populated
  sh.import_env_functions();  // pull in any BASH_FUNC_*%% exported functions

  // Invocation name: basename of argv[0], minus a leading '-' (which marks a
  // login shell).  Drives error-message prefixes and startup-file names.
  std::string prog = argc > 0 ? argv[0] : "gnash";
  bool login = !prog.empty() && prog[0] == '-';
  std::string base = login ? prog.substr(1) : prog;
  auto slash = base.rfind('/');
  if (slash != std::string::npos) base = base.substr(slash + 1);
  if (base.empty()) base = "gnash";
  sh.shell_name = base;
  // $0 defaults to argv[0] (as bash: `bash -c' shows "bash", "/bin/bash", etc.);
  // a script path or a -c name argument overrides it below.
  sh.arg0 = prog;
  // Invoked as rbash / rsh / rksh (a leading `r'): a restricted shell.
  {
    auto sl = base.rfind('/');
    std::string nm = sl == std::string::npos ? base : base.substr(sl + 1);
    if (nm == "rbash" || nm == "rsh" || nm == "rksh" || nm == "rgnash")
      sh.opt_restricted = true;
  }
  const std::string prefix = base;

  // $SHELL is the execution path of the current shell (persona-independent).
  std::string exec_path = resolve_exec_path(prog);
  sh.self_exe = exec_path;  // for re-execing #!-less scripts (ENOEXEC)
  sh.set_exported("SHELL", exec_path);

  std::vector<std::string> args(argv + 1, argv + argc);

  // ---- option parsing ----------------------------------------------------
  StartupOpts sopts;
  bool force_interactive = false;
  bool force_stdin = false;
  bool have_c = false;
  std::string personality_flag;  // --personality=<name>, overrides invocation name
  size_t idx = 0;
  for (; idx < args.size(); idx++) {
    const std::string &a = args[idx];
    if (a == "--") { idx++; break; }
    if (a.size() < 2 || (a[0] != '-' && a[0] != '+')) break;  // not an option
    if (a == "-") break;  // "-" alone: not a script; falls through to stdin

    if (a[0] == '-' && a[1] == '-') {  // long option
      std::string lo = a.substr(2);
      std::string val;
      auto eq = lo.find('=');
      if (eq != std::string::npos) { val = lo.substr(eq + 1); lo = lo.substr(0, eq); }
      if (lo == "login") login = true;
      else if (lo == "norc") sopts.norc = true;
      else if (lo == "noprofile") sopts.noprofile = true;
      else if (lo == "posix" || lo == "noediting") { /* accepted, ignored */ }
      else if (lo == "rcfile" || lo == "init-file") {
        sopts.rcfile = !val.empty() ? val : (idx + 1 < args.size() ? args[++idx] : "");
      } else if (lo == "personality") {
        personality_flag = !val.empty() ? val : (idx + 1 < args.size() ? args[++idx] : "");
      } else if (lo == "version") {
        std::printf("gnash, version " GNASH_VERSION " (bash-compatible reimplementation)\n");
        return 0;
      } else if (lo == "help") {
        std::printf("usage: %s [options] [script [args]]\n", prefix.c_str());
        return 0;
      } else {
        std::fprintf(stderr, "%s: %s: invalid option\n", sh.shell_name.c_str(), a.c_str());
        return 2;
      }
      continue;
    }

    // short options, possibly grouped; '+' unsets
    bool set = (a[0] == '-');
    bool stop_after = false;
    for (size_t k = 1; k < a.size(); k++) {
      char o = a[k];
      switch (o) {
        case 'l': login = true; break;
        case 'i': force_interactive = true; break;
        case 's': force_stdin = true; break;
        case 'e': sh.opt_errexit = set; break;
        case 'x': sh.opt_xtrace = set; break;
        case 'u': sh.opt_nounset = set; break;
        case 'f': sh.opt_noglob = set; break;
        case 'v': sh.opt_verbose = set; break;
        case 'n': sh.opt_noexec = set; break;  // read but don't execute
        case 'r': if (set) sh.opt_restricted = true; break;  // restricted shell
        case 'm': case 'B': case 'h': case 'H':
          break;  // accepted, not (yet) acted on
        // `-c' takes its command from the next word, but other flags grouped in
        // the same word still apply -- `-ce cmd' means both `-c' and `-e'.  So
        // mark have_c and keep scanning the remaining letters of this word.
        case 'c': have_c = true; break;
        case 'o': {
          std::string name = (k + 1 < a.size()) ? a.substr(k + 1)
                             : (idx + 1 < args.size() ? args[++idx] : "");
          apply_set_o(sh, name, set);
          stop_after = true;  // -o consumed the rest of this word / next word
          break;
        }
        default:
          std::fprintf(stderr, "%s: -%c: invalid option\n", sh.shell_name.c_str(), o);
          return 2;
      }
      if (stop_after) break;
    }
    if (have_c) { idx++; break; }
    if (stop_after) continue;
  }
  sh.login_shell = login;
  // Keep the `login_shell' shopt in sync: it was seeded from the (still false)
  // default before the invocation was parsed, so refresh it now.
  sh.shopt_opts["login_shell"] = login;

  // The personality: the --personality flag wins, else the invocation name.
  configure_persona(sh, personality_flag.empty() ? prefix : personality_flag, exec_path);

  // Startup (and logout) files follow the shell's *identity* -- the personality
  // name if given, else the invocation name.  So `gnash --personality=bash'
  // reads ~/.bash_profile / ~/.bashrc (not ~/.gnash*), just as a `bash'-named
  // symlink would.  (Error messages still use the invocation name via
  // sh.shell_name.)
  const std::string &startup_prefix = sh.personality_name;

  // ---- dispatch ----------------------------------------------------------
  if (have_c) {
    sh.invocation_char = 'c';  // $- includes `c'
    std::string cmd = idx < args.size() ? args[idx++] : "";
    if (idx < args.size()) {
      sh.arg0 = args[idx];
      sh.shell_name = args[idx];  // $0 also names the shell in error messages
      sh.positional.assign(args.begin() + idx + 1, args.end());
    }
    sh.init_job_control(false);
    read_startup_files(sh, startup_prefix, login, false, sopts);
    sh.run_string(cmd);
  } else if (!force_stdin && idx < args.size()) {
    std::ifstream f(args[idx]);
    if (!f) {
      std::fprintf(stderr, "%s: %s: %s\n", sh.shell_name.c_str(), args[idx].c_str(),
                   std::strerror(errno));
      return 127;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    sh.arg0 = args[idx];
    sh.shell_name = args[idx];  // scripts report errors as "SCRIPT: line N: ..."
    sh.positional.assign(args.begin() + idx + 1, args.end());
    sh.init_job_control(false);
    read_startup_files(sh, startup_prefix, login, false, sopts);
    // Base frame for BASH_SOURCE[0]/$0 = the script path, as bash reports it.
    sh.push_src_frame("main", args[idx], 0, false);
    sh.run_script_lines(ss.str());
  } else {
    // Read commands from standard input.
    sh.invocation_char = 's';  // $- includes `s' when reading from stdin
    if (idx < args.size())  // `-s' with trailing args: they are positionals
      sh.positional.assign(args.begin() + idx, args.end());
    bool interactive = force_interactive || isatty(STDIN_FILENO);
    if (interactive) {
      sh.interactive = true;
      sh.init_job_control(true);
      // Default prompts (set before startup files so an rc file can override):
      // user@host:cwd followed by `$' (or `#' for root), and `> ' for
      // continuation lines.
      if (sh.is_zsh()) {
        // zsh prompt escapes use `%': user@host:cwd then %/# (root).
        if (!sh.is_set("PS1")) sh.set("PS1", "%n@%m:%~%# ");
        if (!sh.is_set("PS2")) sh.set("PS2", "%_> ");
      } else if (sh.is_csh()) {
        // csh prompt escapes also use `%': host:cwd then %# (`#' for root).
        if (!sh.is_set("PS1")) sh.set("PS1", "%m:%~%# ");
        if (!sh.is_set("PS2")) sh.set("PS2", "? ");
      } else if (sh.is_ash() || sh.is_ksh()) {
        // ash/ksh/POSIX: a plain "$ " prompt ("# " for root).
        if (!sh.is_set("PS1")) sh.set("PS1", getuid() == 0 ? "# " : "$ ");
        if (!sh.is_set("PS2")) sh.set("PS2", "> ");
      } else {
        if (!sh.is_set("PS1")) sh.set("PS1", "\\u@\\h:\\w\\$ ");
        if (!sh.is_set("PS2")) sh.set("PS2", "> ");
      }
      // History defaults for interactive shells (bash sets these; a startup
      // file may override them).  HISTFILE follows the personality, e.g. the
      // bash persona uses ~/.bash_history, gnash uses ~/.gnash_history.
      if (!sh.is_set("HISTSIZE")) sh.set("HISTSIZE", "500");
      if (!sh.is_set("HISTFILESIZE")) sh.set("HISTFILESIZE", "500");
      if (!sh.is_set("HISTFILE")) {
        const char *home = std::getenv("HOME");
        if (home) sh.set("HISTFILE", std::string(home) + "/." + startup_prefix + "_history");
      }
      read_startup_files(sh, startup_prefix, login, true, sopts);
      return gnash::core::run_interactive(sh);
    }
    sh.init_job_control(false);
    read_startup_files(sh, startup_prefix, login, false, sopts);
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    sh.run_script_lines(ss.str());
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
  if (login && !sopts.noprofile) {
    const char *home = std::getenv("HOME");
    if (home) source_if_exists(sh, std::string(home) + "/." + prefix + "_logout");
  }
  return rc;
}
