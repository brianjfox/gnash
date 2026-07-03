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
// invoked as "bash" it reads ~/.bash_profile / ~/.bashrc / $BASH_ENV.
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

#include "gnash/core/shell.hpp"

namespace {

using gnash::core::Shell;

bool file_readable(const std::string &path) { return access(path.c_str(), R_OK) == 0; }

void source_if_exists(Shell &sh, const std::string &path) {
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
void read_startup_files(Shell &sh, const std::string &prefix, bool login, bool interactive,
                        const StartupOpts &o) {
  const char *home = std::getenv("HOME");
  std::string h = home ? home : "";

  if (login) {
    if (!o.noprofile) {
      source_if_exists(sh, "/etc/profile");
      if (!h.empty()) {
        const std::string candidates[] = {h + "/." + prefix + "_profile",
                                          h + "/." + prefix + "_login", h + "/.profile"};
        for (const std::string &c : candidates)
          if (file_readable(c)) { source_if_exists(sh, c); break; }
      }
    }
  } else if (interactive) {
    if (!o.norc) {
      if (!o.rcfile.empty()) {
        source_if_exists(sh, o.rcfile);
      } else {
        source_if_exists(sh, "/etc/" + prefix + "." + prefix + "rc");
        if (!h.empty()) source_if_exists(sh, h + "/." + prefix + "rc");
      }
    }
  } else {
    const char *env = std::getenv((upper(prefix) + "_ENV").c_str());
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
  // Other -o names (pipefail, posix, vi, emacs, ...) are accepted and ignored.
}

}  // namespace

int main(int argc, char **argv) {
  Shell sh;

  // Invocation name: basename of argv[0], minus a leading '-' (which marks a
  // login shell).  Drives error-message prefixes and startup-file names.
  std::string prog = argc > 0 ? argv[0] : "gnash";
  bool login = !prog.empty() && prog[0] == '-';
  std::string base = login ? prog.substr(1) : prog;
  auto slash = base.rfind('/');
  if (slash != std::string::npos) base = base.substr(slash + 1);
  if (base.empty()) base = "gnash";
  sh.shell_name = base;
  const std::string prefix = base;

  // Compatibility variables many rc files inspect.  $BASH is the full pathname
  // used to invoke this shell (whatever its name), as in bash.
  sh.set("BASH", resolve_exec_path(prog));
  sh.set("BASH_VERSION", "5.3.0(1)-release");
  {
    // BASH_VERSINFO: major minor patch build status machtype (readonly array).
    std::string mach = "unknown";
    struct utsname u;
    if (uname(&u) == 0) {
      std::string sys = u.sysname;
      for (char &c : sys) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      std::string vendor = (sys == "darwin") ? "apple" : "unknown";
      mach = std::string(u.machine) + "-" + vendor + "-" + sys + u.release;
    }
    std::vector<std::pair<std::optional<std::string>, std::string>> vi = {
        {std::nullopt, "5"}, {std::nullopt, "3"},       {std::nullopt, "0"},
        {std::nullopt, "1"}, {std::nullopt, "release"}, {std::nullopt, mach}};
    sh.array_assign("BASH_VERSINFO", vi, false, false);
    sh.vars["BASH_VERSINFO"].readonly = true;
    sh.set("MACHTYPE", mach);
  }

  std::vector<std::string> args(argv + 1, argv + argc);

  // ---- option parsing ----------------------------------------------------
  StartupOpts sopts;
  bool force_interactive = false;
  bool force_stdin = false;
  bool have_c = false;
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
      } else if (lo == "version") {
        std::printf("gnash, version 0.1 (bash-compatible reimplementation)\n");
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
        case 'n': case 'r': case 'm': case 'B': case 'h': case 'H':
          break;  // accepted, not (yet) acted on
        case 'c': have_c = true; stop_after = true; break;
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

  // ---- dispatch ----------------------------------------------------------
  if (have_c) {
    std::string cmd = idx < args.size() ? args[idx++] : "";
    if (idx < args.size()) {
      sh.arg0 = args[idx];
      sh.positional.assign(args.begin() + idx + 1, args.end());
    }
    sh.init_job_control(false);
    read_startup_files(sh, prefix, login, false, sopts);
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
    read_startup_files(sh, prefix, login, false, sopts);
    sh.run_string(ss.str());
  } else {
    // Read commands from standard input.
    if (idx < args.size())  // `-s' with trailing args: they are positionals
      sh.positional.assign(args.begin() + idx, args.end());
    bool interactive = force_interactive || isatty(STDIN_FILENO);
    if (interactive) {
      sh.interactive = true;
      sh.init_job_control(true);
      read_startup_files(sh, prefix, login, true, sopts);
      return gnash::core::run_interactive(sh);
    }
    sh.init_job_control(false);
    read_startup_files(sh, prefix, login, false, sopts);
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
  if (login && !sopts.noprofile) {
    const char *home = std::getenv("HOME");
    if (home) source_if_exists(sh, std::string(home) + "/." + prefix + "_logout");
  }
  return rc;
}
