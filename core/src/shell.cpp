// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// shell.cpp -- interpreter state and top-level run/capture.

#include "gnash/core/shell.hpp"

#include <algorithm>
#include <set>
#include <clocale>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <ctime>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#include "readline/history.h"
#include "strmatch.h"

#include "gnash/core/csh.hpp"
#include "gnash/core/executor.hpp"
#include "gnash/core/expand.hpp"
#include "gnash/core/parser.hpp"

extern "C" char **environ;

namespace gnash::core {

bool apply_set_o_option(Shell &sh, const std::string &o, bool on);

namespace { const char *signum_to_trapname(int sig); }

Shell::Shell() {
  // Import the process environment as exported shell variables.
  for (char **e = environ; e && *e; e++) {
    const char *eq = std::strchr(*e, '=');
    if (!eq) continue;
    std::string name(*e, static_cast<size_t>(eq - *e));
    Variable var;
    var.value = std::string(eq + 1);
    var.exported = true;
    vars[name] = var;
  }
  if (!is_set("IFS")) set("IFS", " \t\n");
  if (is_set("POSIXLY_CORRECT")) opt_posix = true;  // inherited from the environment
  if (get("BASHLY_CORRECT") == "true") apply_bashly_correct(true);  // likewise
  set("OPTIND", "1");  // bash initializes getopts state at startup
  set("PPID", std::to_string(static_cast<long>(getppid())));
  set("$", std::to_string(static_cast<long>(getpid())));
  // bash exposes the real/effective user id as readonly integer variables.
  const std::initializer_list<std::pair<const char *, unsigned int>> uid_vars = {
    {"UID", static_cast<unsigned int>(getuid())},
    {"EUID", static_cast<unsigned int>(geteuid())}
  };
  for (const auto &uv : uid_vars) {
    set(uv.first, std::to_string(static_cast<long>(uv.second)));
    Variable &v = vars[uv.first];
    v.integer = true;
    v.readonly = true;
  }
  seconds_base = static_cast<long long>(std::time(nullptr));  // $SECONDS origin

  // Establish a valid logical $PWD.  Keep the inherited (possibly symlinked)
  // value when it still names the current directory, as bash does; otherwise
  // fall back to the resolved path.
  char cwd[4096];
  if (getcwd(cwd, sizeof cwd)) {
    std::string pwd = get("PWD");
    struct stat a, b;
    bool valid = !pwd.empty() && pwd[0] == '/' && stat(pwd.c_str(), &a) == 0 &&
                 stat(cwd, &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
    if (valid) vars["PWD"].exported = true;
    else set_exported("PWD", cwd);
  }

  // Signals inherited as SIG_IGN keep that disposition, and bash reports them
  // through `trap' as `trap -- '' SIG'.  Record them so the listing matches.
  for (int s = 1; s < NSIG; s++) {
    if (s == SIGKILL || s == SIGSTOP) continue;
    const char *nm = signum_to_trapname(s);
    if (!nm) continue;
    struct sigaction cur;
    if (sigaction(s, nullptr, &cur) == 0 && cur.sa_handler == SIG_IGN) traps[nm] = "";
  }
}

// Advance bash's RANDOM generator (Park-Miller minimal-standard PRNG) and
// return a value in 0..32767, matching bash 5.3 exactly for a given seed.
// Apply a `declare -u'/`-l'/`-c' case attribute to a value (character-aware).
static std::string fold_case(std::string s, bool up, bool lo, bool cap) {
  if (up) return mb_upper(s);
  if (lo) return mb_lower(s);
  if (cap) return mb_capitalize(s);
  return s;
}

int Shell::next_random() {
  if (!rand_seeded) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    rand_seed = static_cast<unsigned long>(tv.tv_sec) ^
                (static_cast<unsigned long>(tv.tv_usec) << 16) ^
                static_cast<unsigned long>(getpid());
    rand_seeded = true;
  }
  unsigned long r = rand_seed ? rand_seed : 123459876UL;
  long h = static_cast<long>(r / 127773UL);
  long l = static_cast<long>(r % 127773UL);
  long t = 16807L * l - 2836L * h;
  if (t < 0) t += 0x7fffffffL;
  rand_seed = static_cast<unsigned long>(t);
  return static_cast<int>(((rand_seed >> 16) ^ (rand_seed & 0xffff)) & 0x7fff);
}

// The dynamic special variables (kept in sync with dynamic_var), exposed so
// completion can offer them even though they are not stored in `vars'.
const std::vector<std::string> &Shell::special_var_names() {
  static const std::vector<std::string> names = {
      "RANDOM", "SECONDS", "LINENO", "BASHPID", "BASH_SUBSHELL",
      "EPOCHSECONDS", "EPOCHREALTIME", "BASH_MONOSECONDS", "HISTCMD",
      "BASHOPTS", "SHELLOPTS", "BASH_ALIASES", "BASH_CMDS", "BASH_ARGC", "BASH_ARGV"};
  return names;
}

// The computed variables that carry bash's att_noassign: an assignment to one
// succeeds and changes nothing, and the dynamic value keeps being reported.
// BASH_SUBSHELL and BASH_TRAPSIG are deliberately absent -- bash does let those
// be overwritten -- as are BASHOPTS/SHELLOPTS, which are readonly instead.
bool Shell::noassign_var(const std::string &n) const {
  if (is_zsh()) return false;  // zsh has no such variables
  static const std::set<std::string> kNoAssign = {
      "GROUPS",   "FUNCNAME",     "LINENO",       "BASHPID",
      "HISTCMD",  "BASH_COMMAND", "BASH_ARGC",    "BASH_ARGV",
      "EPOCHSECONDS", "EPOCHREALTIME", "BASH_MONOSECONDS"};
  return kNoAssign.count(n) != 0;
}

// Defined in builtins.cpp: the state of every `set -o' option, in name order.
std::vector<std::pair<std::string, bool>> set_option_states(Shell &sh);

// Dynamic variables computed on each reference.  Returns false for names that
// are not dynamic (the caller then looks them up as ordinary variables).
bool Shell::dynamic_var(const std::string &name, std::string &out) {
  if (name == "RANDOM") { out = std::to_string(next_random()); return true; }
  if (name == "SECONDS") {
    out = std::to_string(static_cast<long long>(std::time(nullptr)) - seconds_base);
    return true;
  }
  if (name == "LINENO") { out = std::to_string(cur_lineno); return true; }
  if (name == "BASHPID") { out = std::to_string(static_cast<long>(getpid())); return true; }
  if (name == "BASH_ARGV0") { out = arg0; return true; }  // reflects $0; always set
  if (name == "BASH_COMMAND") { out = bash_command; return true; }  // command being run
  if (name == "BASH_TRAPSIG") { out = trap_sig ? std::to_string(trap_sig) : ""; return true; }
  if (name == "HISTCMD") { out = std::to_string(history_length); return true; }  // history number
  if (name == "BASH_SUBSHELL") { out = std::to_string(subshell_level); return true; }
  if (name == "EPOCHSECONDS") {
    out = std::to_string(static_cast<long long>(std::time(nullptr)));
    return true;
  }
  if (name == "EPOCHREALTIME") {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    char b[32];
    std::snprintf(b, sizeof b, "%lld.%06d", static_cast<long long>(tv.tv_sec),
                  static_cast<int>(tv.tv_usec));
    out = b;
    return true;
  }
  if (name == "BASH_MONOSECONDS") {  // seconds from the monotonic clock (bash 5.3)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    out = std::to_string(static_cast<long long>(ts.tv_sec));
    return true;
  }
  if (name == "BASHOPTS") {  // colon-separated list of the enabled shopt options
    std::string r;
    for (const auto &kv : shopt_opts) if (kv.second) { if (!r.empty()) r += ':'; r += kv.first; }
    out = r;
    return true;
  }
  if (name == "SHELLOPTS") {  // colon-separated list of the enabled `set -o' options
    std::string r;
    for (const auto &o : set_option_states(*this))
      if (o.second) { if (!r.empty()) r += ':'; r += o.first; }
    out = r;
    return true;
  }
  return false;
}

// The call-stack argument arrays $BASH_ARGC / $BASH_ARGV, populated only under
// `shopt -s extdebug'.  Both list the innermost frame first, with a trailing
// entry for the base ("main") script.  In $BASH_ARGV each frame's arguments
// appear in reverse order (bash builds it by pushing args as they are seen).
std::vector<std::string> Shell::bash_argc_view() const {
  std::vector<std::string> out;
  if (argframes.empty()) return out;  // only exists while in a function w/ extdebug
  for (auto it = argframes.rbegin(); it != argframes.rend(); ++it)
    out.push_back(std::to_string(static_cast<int>(it->size())));
  out.push_back(std::to_string(static_cast<int>(top_positionals.size())));
  return out;
}
std::vector<std::string> Shell::bash_argv_view() const {
  std::vector<std::string> out;
  if (argframes.empty()) return out;
  for (auto it = argframes.rbegin(); it != argframes.rend(); ++it)
    for (auto a = it->rbegin(); a != it->rend(); ++a) out.push_back(*a);
  for (auto it = top_positionals.rbegin(); it != top_positionals.rend(); ++it) out.push_back(*it);
  return out;
}

// BASH_ALIASES/BASH_CMDS/BASH_ARGC/BASH_ARGV present live shell tables as
// arrays.  Fills PAIRS (ordered key,value) and returns true for those names.
bool Shell::virtual_array(const std::string &name,
                          std::vector<std::pair<std::string, std::string>> &pairs) const {
  if (name == "BASH_ALIASES") {
    for (const auto &k : aliases_order()) pairs.emplace_back(k, aliases.at(k));
    return true;
  }
  if (name == "BASH_CMDS") {
    for (const auto &k : hashed_order()) pairs.emplace_back(k, hashed.at(k));
    return true;
  }
  if (name == "DIRSTACK") {
    auto v = dirstack();
    for (size_t i = 0; i < v.size(); i++) pairs.emplace_back(std::to_string(i), v[i]);
    return true;
  }
  if (name == "BASH_ARGC" || name == "BASH_ARGV") {
    auto v = (name == "BASH_ARGC") ? bash_argc_view() : bash_argv_view();
    for (size_t i = 0; i < v.size(); i++) pairs.emplace_back(std::to_string(i), v[i]);
    return true;
  }
  return false;
}

// C-c during command execution (interactive): the handler installed by the REPL
// sets this, and the executor's unwinding() check aborts the running command.
volatile std::sig_atomic_t g_sigint_received = 0;

namespace {
volatile sig_atomic_t g_trap_pending[NSIG];

void trap_signal_handler(int sig) {
  if (sig > 0 && sig < NSIG) g_trap_pending[sig] = 1;
}

// Canonical trap name for a signal number (matching how `trap' stores keys).
const char *signum_to_trapname(int sig) {
  switch (sig) {
    case SIGHUP: return "HUP";   case SIGINT: return "INT";
    case SIGQUIT: return "QUIT"; case SIGILL: return "ILL";
    case SIGTRAP: return "TRAP"; case SIGABRT: return "ABRT";
    case SIGFPE: return "FPE";   case SIGBUS: return "BUS";
    case SIGSEGV: return "SEGV"; case SIGSYS: return "SYS";
    case SIGPIPE: return "PIPE"; case SIGALRM: return "ALRM";
    case SIGTERM: return "TERM"; case SIGURG: return "URG";
    case SIGTSTP: return "TSTP"; case SIGCONT: return "CONT";
    case SIGCHLD: return "CHLD"; case SIGTTIN: return "TTIN";
    case SIGTTOU: return "TTOU"; case SIGXCPU: return "XCPU";
    case SIGXFSZ: return "XFSZ"; case SIGVTALRM: return "VTALRM";
    case SIGPROF: return "PROF"; case SIGWINCH: return "WINCH";
    case SIGUSR1: return "USR1"; case SIGUSR2: return "USR2";
    default: return nullptr;
  }
}
}  // namespace

void Shell::set_signal_trap(int signo, bool active) {
  if (signo <= 0 || signo >= NSIG) return;
  // SIGCHLD is reaped synchronously and its trap is delivered from the reap
  // counter (note_child_reaped), so leave its disposition at the default rather
  // than installing the async handler -- otherwise the trap would fire twice.
  if (signo == SIGCHLD) return;
  struct sigaction sa;
  std::memset(&sa, 0, sizeof sa);
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;  // let blocking waits resume; traps run between commands
  sa.sa_handler = active ? trap_signal_handler : SIG_DFL;
  sigaction(signo, &sa, nullptr);
}

// Re-arm the handlers for every currently-trapped signal so a blocking wait is
// interrupted (EINTR) instead of resumed.  `restart' selects the normal
// SA_RESTART disposition (used to restore after the wait) vs. no-restart.
static void rearm_trap_handlers(const std::map<std::string, std::string> &traps,
                                bool restart) {
  for (int sig = 1; sig < NSIG; sig++) {
    if (sig == SIGCHLD) continue;                  // reaped synchronously, no async handler
    const char *nm = signum_to_trapname(sig);      // skip EXIT/DEBUG/ERR/RETURN pseudo-traps
    if (!nm) continue;
    auto it = traps.find(nm);
    if (it == traps.end() || it->second.empty()) continue;  // untrapped / reset
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = restart ? SA_RESTART : 0;
    sa.sa_handler = trap_signal_handler;
    sigaction(sig, &sa, nullptr);
  }
}

void Shell::begin_interruptible_wait() { rearm_trap_handlers(traps, false); }
void Shell::end_interruptible_wait() { rearm_trap_handlers(traps, true); }

int Shell::pending_trapped_signal() {
  for (int s = 1; s < NSIG; s++) {
    if (!g_trap_pending[s]) continue;
    const char *nm = signum_to_trapname(s);
    if (!nm) continue;
    auto it = traps.find(nm);
    if (it != traps.end() && !it->second.empty()) return s;
  }
  return 0;
}

int Shell::run_debug_trap(const std::string &cmd_text) {
  auto it = traps.find("DEBUG");
  if (it == traps.end() || in_debug_trap) return 0;
  // Without functrace a function does not inherit the DEBUG trap: inside one it
  // fires only if the function installed its own (the body differs from what it
  // was when the innermost function was entered).
  if (!opt_functrace && in_function() &&
      (debug_frame.empty() || it->second == debug_frame.back()))
    return 0;
  in_debug_trap = true;
  bash_command = cmd_text;
  int saved = last_status;  // $? inside the trap is the previous command's status
  int saved_line = cur_lineno;  // the trap must not leak its own line numbers
  // bash runs a DEBUG/RETURN/ERR trap body without resetting the line counter
  // (no SEVAL_RESETLINE), so $LINENO in the body's first line reports the
  // command that triggered the trap; later body lines count up from there.
  int saved_base = lineno_base;
  lineno_base = cur_lineno - 1;
  std::string body = it->second;
  int st = run_string(body);
  last_status = saved;  // the trap does not alter $? for the upcoming command
  cur_lineno = saved_line;
  lineno_base = saved_base;
  in_debug_trap = false;
  return st;
}

void Shell::run_err_trap(int status) {
  auto it = traps.find("ERR");
  if (it == traps.end() || it->second.empty() || in_err_trap) return;
  // Without errtrace (`set -E'), the ERR trap is not inherited into function
  // bodies; subshell non-inheritance is already modeled by the fork-time
  // trap erasure, so a trap SET INSIDE the subshell still fires
  // (redir12.sub's \`(trap ... ERR; while ...)').
  if (!opt_functrace && in_function()) return;
  in_err_trap = true;
  std::string saved_cmd = bash_command;  // restored below; frozen during the body
  int saved = last_status;
  last_status = status;  // $? inside the ERR trap is the failing command's status
  std::string body = it->second;
  run_string(body);
  last_status = saved;
  bash_command = saved_cmd;
  in_err_trap = false;
}

int Shell::run_return_trap(int status) {
  auto it = traps.find("RETURN");
  if (it == traps.end() || it->second.empty() || in_return_trap) return 0;
  in_return_trap = true;
  int saved = last_status;
  last_status = status;  // $? inside the RETURN trap is the function's return status
  std::string body = it->second;
  int st = run_string(body);
  last_status = saved;
  in_return_trap = false;
  return st;
}

void Shell::note_child_reaped() {
  // bash runs the SIGCHLD trap once for each terminated child; only count while
  // such a trap is installed.
  if (traps.count("CHLD")) pending_sigchld++;
}

void Shell::run_pending_traps() {
  if (in_trap) return;
  // The SIGCHLD trap fires once per child reaped since the last check.
  while (pending_sigchld > 0) {
    auto it = traps.find("CHLD");
    if (it == traps.end() || it->second.empty()) { pending_sigchld = 0; break; }
    pending_sigchld--;
    in_trap = true;
    int saved = last_status;
    std::string cmd = it->second;
    run_string(cmd);
    last_status = saved;
    in_trap = false;
  }
  for (int s = 1; s < NSIG; s++) {
    if (!g_trap_pending[s]) continue;
    g_trap_pending[s] = 0;
    const char *nm = signum_to_trapname(s);
    if (!nm) continue;
    auto it = traps.find(nm);
    if (it == traps.end() || it->second.empty()) continue;
    in_trap = true;
    int saved = last_status;  // the interrupted command's $?
    int saved_sig = trap_sig;
    trap_sig = s;             // $BASH_TRAPSIG names the delivering signal
    std::string cmd = it->second;
    run_string(cmd);
    trap_sig = saved_sig;
    last_status = saved;
    in_trap = false;
  }
}

void Shell::reap_procsubs(size_t from) {
  for (size_t k = from; k < procsubs.size(); k++) {
    if (procsubs[k].fd >= 0) close(procsubs[k].fd);
    int st = 0;
    if (waitpid(static_cast<pid_t>(procsubs[k].pid), &st, 0) > 0) {
      // Remember the status so a later `wait "$!"' can report it: bash keeps
      // process-substitution children waitable after the command that
      // created them finishes (procsub1.sub).
      int rc = WIFEXITED(st) ? WEXITSTATUS(st)
                             : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
      reaped_procsub_status[procsubs[k].pid] = rc;
    }
  }
  if (from < procsubs.size()) procsubs.resize(from);
}

// bash caps nameref chains at NAMEREF_MAX (8) links.  gnash allows more by
// default -- a long chain is a legitimate, if unusual, thing to build -- and
// exposes the limit as $GNASH_NAMEREF_MAX so a script that wants bash's exact
// behaviour can set it to 8.  A missing, malformed or non-positive value falls
// back to the default.
int Shell::nameref_max() const {
  constexpr int kDefault = 100;
  auto it = vars.find("GNASH_NAMEREF_MAX");
  if (it == vars.end()) return kDefault;
  char *end = nullptr;
  long v = std::strtol(it->second.value.c_str(), &end, 10);
  if (end == it->second.value.c_str() || *end != '\0' || v < 1) return kDefault;
  return static_cast<int>(v);
}

// Turning $BASHLY_CORRECT on pins $GNASH_NAMEREF_MAX to bash's 8, remembering
// what was there so turning it off restores it -- including restoring "unset".
// Nothing happens unless the state actually changes, so repeated assignments of
// the same value cannot lose the saved value.  The writes go straight to `vars'
// to avoid recursing back through Shell::set.
void Shell::apply_bashly_correct(bool on) {
  if (on == bashly_correct) return;
  bashly_correct = on;
  if (on) {
    auto it = vars.find("GNASH_NAMEREF_MAX");
    saved_nameref_max = (it == vars.end() || it->second.invisible)
                            ? std::nullopt
                            : std::optional<std::string>(it->second.value);
    Variable &v = vars["GNASH_NAMEREF_MAX"];
    v.value = "8";
    v.invisible = false;
    return;
  }
  if (saved_nameref_max) {
    Variable &v = vars["GNASH_NAMEREF_MAX"];
    v.value = *saved_nameref_max;
    v.invisible = false;
  } else {
    vars.erase("GNASH_NAMEREF_MAX");
  }
  saved_nameref_max.reset();
}

std::string Shell::deref(const std::string &n) const {
  std::string cur = n;
  const int max_links = nameref_max();
  for (int guard = 0; guard < max_links; guard++) {
    auto it = vars.find(cur);
    if (it == vars.end() || !it->second.nameref) return cur;
    const std::string &tgt = it->second.value;
    if (tgt.empty() || tgt == cur) return cur;  // self/empty ref: stop
    cur = tgt;
  }
  return cur;
}

std::string Shell::deref_ex(const std::string &n, bool &circular, bool *too_deep) const {
  circular = false;
  if (too_deep) *too_deep = false;
  std::string cur = n;
  std::set<std::string> seen;
  const int max_links = nameref_max();
  for (int links = 0;; links++) {
    auto it = vars.find(cur);
    if (it == vars.end() || !it->second.nameref) return cur;
    const std::string &tgt = it->second.value;
    if (tgt.empty()) return cur;  // untargeted nameref: not circular
    if (tgt == cur || seen.count(tgt)) {  // self reference or a longer loop
      circular = true;
      return cur;
    }
    // One link too many.  The chain is not circular -- it simply runs deeper
    // than the shell is willing to follow -- so it resolves to nothing.
    if (links >= max_links) {
      if (too_deep) *too_deep = true;
      return cur;
    }
    seen.insert(cur);
    cur = tgt;
  }
}

Variable &Shell::global_var_ref(const std::string &name) {
  for (auto &scope : local_stack) {  // front = outermost (nearest to global)
    for (auto &e : scope)
      if (e.first == name) {
        if (!e.second) e.second = Variable{};  // global didn't exist; create it
        return *e.second;
      }
  }
  return vars[name];
}

const Variable *Shell::global_var_ptr(const std::string &name) const {
  for (const auto &scope : local_stack) {  // front = outermost
    for (const auto &e : scope)
      if (e.first == name) return e.second ? &*e.second : nullptr;
  }
  auto it = vars.find(name);
  return it == vars.end() ? nullptr : &it->second;
}

bool Shell::valid_nameref_target(const std::string &s) {
  if (s.empty()) return false;
  if (!(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
  size_t i = 1;
  for (; i < s.size(); i++) {
    char c = s[i];
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') continue;
    if (c == '[') break;  // an array subscript may follow the identifier
    return false;
  }
  // A subscript, if present, must be non-empty and close the string
  // (`foo[x]': `[' at i, `]' last, at least one char between).
  if (i < s.size() && s[i] == '[')
    return s.back() == ']' && s.size() - i > 2;
  return true;
}

bool Shell::nameref_elt(const std::string &n_in, std::string &base,
                        std::string &sub) const {
  // Only a nameref may introduce a subscript.  A name that already contains a
  // `[' is a direct element reference (`a[0]', handled by array_get/array_set),
  // not a nameref-to-element, and must not be rerouted here.
  if (n_in.find('[') != std::string::npos) return false;
  std::string d = deref(n_in);
  // A `[' in the dereferenced name can only appear because a nameref in the
  // chain pointed at a subscripted target (`declare -n r=a[2]').
  size_t lb = d.find('[');
  if (lb == 0 || lb == std::string::npos || d.back() != ']') return false;
  base = d.substr(0, lb);
  sub = d.substr(lb + 1, d.size() - lb - 2);
  return true;
}

bool Shell::is_set(const std::string &n_in) const {
  // BASH_ALIASES/BASH_CMDS always exist; BASH_ARGC/BASH_ARGV exist while their
  // (extdebug-only) view is non-empty.
  if (n_in == "BASH_ALIASES" || n_in == "BASH_CMDS") return true;
  if (n_in == "BASH_ARGC" || n_in == "BASH_ARGV") return !argframes.empty();
  std::string base, sub;
  if (nameref_elt(n_in, base, sub)) {
    // A nameref to an array element is "set" iff that element exists.
    auto it = vars.find(base);
    if (it == vars.end()) return false;
    const Variable &v = it->second;
    if (v.kind == VarKind::Assoc) return v.assoc.count(sub) != 0;
    bool ok = true;
    long long k = eval_arith(const_cast<Shell &>(*this), sub, &ok);
    if (!ok) k = 0;
    if (v.kind == VarKind::Indexed) return v.idx.count(k) != 0;
    return k == 0;  // a scalar's element [0] is the scalar itself
  }
  std::string n = deref(n_in);
  auto it = vars.find(n);
  if (it == vars.end()) return false;
  // A nameref with no (or a self) target -- deref stops on it -- is unset.
  if (it->second.nameref) return false;
  // A declared-but-unset (invisible) variable is not "set" for `-v'.
  if (it->second.invisible) return false;
  return true;
}

static std::string scalar_of(const Variable &v) {
  if (v.kind == VarKind::Indexed) return v.idx.count(0) ? v.idx.at(0) : std::string();
  if (v.kind == VarKind::Assoc) return v.assoc.count("0") ? v.assoc.at("0") : std::string();
  return v.value;
}

std::string Shell::get(const std::string &n_in) const {
  std::string base, sub;
  if (nameref_elt(n_in, base, sub)) return array_get(base, sub);
  bool circular = false, too_deep = false;
  std::string n = deref_ex(n_in, circular, &too_deep);
  // A chain longer than nameref_max() resolves to nothing, with bash's warning
  // naming the limit actually in force.
  if (too_deep) {
    std::fprintf(stderr, "%swarning: %s: maximum nameref depth (%d) exceeded\n",
                 err_prefix().c_str(), n_in.c_str(), nameref_max());
    return std::string();
  }
  if (circular) {
    // bash warns and, at function scope, resolves the reference at global
    // scope (find_variable_nameref); at global scope it resolves to nothing.
    std::fprintf(stderr, "%swarning: %s: circular name reference\n",
                 err_prefix().c_str(), n_in.c_str());
    const Variable *g = in_function() ? global_var_ptr(n_in) : nullptr;
    return g ? scalar_of(*g) : std::string();
  }
  auto it = vars.find(n);
  return it == vars.end() ? std::string() : scalar_of(it->second);
}

std::string Shell::get_quiet(const std::string &n_in) const {
  std::string base, sub;
  if (nameref_elt(n_in, base, sub)) return array_get(base, sub);
  bool circular = false, too_deep = false;
  std::string n = deref_ex(n_in, circular, &too_deep);
  if (too_deep) return std::string();  // as get(), but this one never reports
  if (circular) {
    const Variable *g = in_function() ? global_var_ptr(n_in) : nullptr;
    return g ? scalar_of(*g) : std::string();
  }
  auto it = vars.find(n);
  return it == vars.end() ? std::string() : scalar_of(it->second);
}

// ---- arrays ---------------------------------------------------------------

// bash's string hash (hashlib.c hash_string): a 32-bit FNV-1-style mix.
static unsigned assoc_hash_string(const std::string &s) {
  unsigned i = 2166136261u;  // FNV_OFFSET
  for (unsigned char c : s) {
    i += (i << 1) + (i << 4) + (i << 7) + (i << 8) + (i << 24);
    i ^= c;
  }
  return i;
}

// Set assoc[key]=val, recording insertion order for a key seen for the first
// time (re-assigning an existing key keeps its original position, as bash's
// hash_search does).
static void assoc_put(Variable &v, const std::string &key, const std::string &val) {
  if (!v.assoc.count(key)) v.assoc_seq.push_back(key);
  v.assoc[key] = val;
}

// Order an associative array's keys the way bash walks its hash table: by
// bucket index (hash & (nbuckets-1)), then, within a bucket, newest key first
// (bash prepends on insert).  Associative arrays use ASSOC_HASH_BUCKETS=1024.
std::vector<std::string> Shell::bash_hash_order(const std::vector<std::string> &insertion,
                                                unsigned buckets) {
  struct E { unsigned bucket; size_t seq; const std::string *key; };
  std::vector<E> es;
  es.reserve(insertion.size());
  for (size_t s = 0; s < insertion.size(); s++)
    es.push_back({assoc_hash_string(insertion[s]) & (buckets - 1), s, &insertion[s]});
  std::stable_sort(es.begin(), es.end(), [](const E &a, const E &b) {
    if (a.bucket != b.bucket) return a.bucket < b.bucket;
    return a.seq > b.seq;  // newest-first within a bucket
  });
  std::vector<std::string> keys;
  keys.reserve(es.size());
  for (const auto &e : es) keys.push_back(*e.key);
  return keys;
}

// Enumeration order of a name->value table given its recorded insertion
// sequence: stale seq entries (erased names) are skipped, untracked names
// (defensive) appended, then bash's bucket layout applied.
static std::vector<std::string> table_order(const std::map<std::string, std::string> &table,
                                            const std::vector<std::string> &seq,
                                            unsigned buckets) {
  std::vector<std::string> ins;
  ins.reserve(table.size());
  for (const auto &k : seq)
    if (table.count(k)) ins.push_back(k);
  for (const auto &kv : table) {
    bool seen = false;
    for (const auto &p : ins) if (p == kv.first) { seen = true; break; }
    if (!seen) ins.push_back(kv.first);
  }
  return Shell::bash_hash_order(ins, buckets);
}

// bash's hashed-command table has 256 buckets (FILENAME_HASH_BUCKETS), the
// alias table 64 (ALIAS_HASH_BUCKETS).
std::vector<std::string> Shell::hashed_order() const { return table_order(hashed, hashed_seq, 256); }
std::vector<std::string> Shell::aliases_order() const { return table_order(aliases, alias_seq, 64); }

std::vector<std::string> Shell::assoc_order(const Variable &v) {
  // Insertion order: keys recorded in assoc_seq, then any stragglers not yet
  // tracked (defensive, so a missed seq update never drops a key).
  std::vector<std::string> ins;
  ins.reserve(v.assoc.size());
  for (const auto &k : v.assoc_seq)
    if (v.assoc.count(k)) ins.push_back(k);
  for (const auto &kv : v.assoc) {
    bool seen = false;
    for (const auto &p : ins) if (p == kv.first) { seen = true; break; }
    if (!seen) ins.push_back(kv.first);
  }
  return bash_hash_order(ins, v.assoc_buckets);
}

std::vector<std::string> Shell::array_values(const std::string &n_in) const {
  std::vector<std::string> out;
  {
    std::vector<std::pair<std::string, std::string>> vp;
    if (virtual_array(n_in, vp)) {
      for (auto &kv : vp) out.push_back(kv.second);
      return out;
    }
  }
  std::string n = deref(n_in);
  auto it = vars.find(n);
  if (it == vars.end()) return out;
  const Variable &v = it->second;
  if (v.kind == VarKind::Indexed)
    for (const auto &kv : v.idx) out.push_back(kv.second);
  else if (v.kind == VarKind::Assoc)
    for (const auto &k : assoc_order(v)) out.push_back(v.assoc.at(k));
  else
    out.push_back(v.value);
  return out;
}

std::vector<std::string> Shell::array_keys(const std::string &n_in) const {
  std::vector<std::string> out;
  {
    std::vector<std::pair<std::string, std::string>> vp;
    if (virtual_array(n_in, vp)) {
      for (auto &kv : vp) out.push_back(kv.first);
      return out;
    }
  }
  std::string n = deref(n_in);
  auto it = vars.find(n);
  if (it == vars.end()) return out;
  const Variable &v = it->second;
  if (v.kind == VarKind::Indexed)
    for (const auto &kv : v.idx) out.push_back(std::to_string(kv.first));
  else if (v.kind == VarKind::Assoc)
    out = assoc_order(v);
  else if (!v.value.empty() || vars.count(n))
    out.push_back("0");
  return out;
}

std::string Shell::array_get(const std::string &n_in, const std::string &sub) const {
  if (n_in == "BASH_ALIASES" || n_in == "BASH_CMDS") {  // string-keyed live tables
    const auto &tbl = (n_in == "BASH_ALIASES") ? aliases : hashed;
    auto it = tbl.find(sub);
    return it != tbl.end() ? it->second : std::string();
  }
  if (n_in == "BASH_ARGC" || n_in == "BASH_ARGV") {  // numeric-indexed views
    auto v = (n_in == "BASH_ARGC") ? bash_argc_view() : bash_argv_view();
    bool ok = true;
    long long k = eval_arith(const_cast<Shell &>(*this), sub, &ok);
    if (!ok) k = 0;
    return (k >= 0 && k < static_cast<long long>(v.size())) ? v[k] : std::string();
  }
  if (n_in == "DIRSTACK") {  // numeric-indexed live directory stack
    auto v = dirstack();
    bool ok = true;
    long long k = eval_arith(const_cast<Shell &>(*this), sub, &ok);
    if (!ok) k = 0;
    return (k >= 0 && k < static_cast<long long>(v.size())) ? v[k] : std::string();
  }
  std::string n = deref(n_in);
  auto it = vars.find(n);
  if (it == vars.end()) return std::string();
  const Variable &v = it->second;
  if (v.kind == VarKind::Assoc) return v.assoc.count(sub) ? v.assoc.at(sub) : std::string();
  bool ok = true;
  long long k = eval_arith(const_cast<Shell &>(*this), sub, &ok);
  if (!ok) k = 0;
  if (v.kind == VarKind::Indexed) {
    // A negative index counts back from the highest set index (bash); under zsh
    // the subscript was already translated (and -1 means "no such element").
    if (k < 0 && !is_zsh() && !v.idx.empty()) k += v.idx.rbegin()->first + 1;
    return v.idx.count(k) ? v.idx.at(k) : std::string();
  }
  return (k == 0) ? v.value : std::string();
}

bool Shell::array_elem_set(const std::string &n_in, const std::string &sub) const {
  if (n_in == "BASH_ALIASES" || n_in == "BASH_CMDS") {  // string-keyed live tables
    const auto &tbl = (n_in == "BASH_ALIASES") ? aliases : hashed;
    return tbl.count(sub) != 0;
  }
  if (n_in == "BASH_ARGC" || n_in == "BASH_ARGV" || n_in == "DIRSTACK") {
    auto v = (n_in == "BASH_ARGC") ? bash_argc_view()
             : (n_in == "BASH_ARGV") ? bash_argv_view() : dirstack();
    bool ok = true;
    long long k = eval_arith(const_cast<Shell &>(*this), sub, &ok);
    if (!ok) k = 0;
    return k >= 0 && k < static_cast<long long>(v.size());
  }
  std::string n = deref(n_in);
  auto it = vars.find(n);
  if (it == vars.end()) return false;
  const Variable &v = it->second;
  if (v.invisible) return false;
  if (v.kind == VarKind::Assoc) return v.assoc.count(sub) != 0;
  bool ok = true;
  long long k = eval_arith(const_cast<Shell &>(*this), sub, &ok);
  if (!ok) k = 0;
  if (v.kind == VarKind::Indexed) {
    if (k < 0 && !is_zsh() && !v.idx.empty()) k += v.idx.rbegin()->first + 1;
    return v.idx.count(k) != 0;
  }
  return k == 0;  // a scalar's only element is itself
}

void Shell::array_set(const std::string &n_in, const std::string &sub, const std::string &val) {
  std::string n = deref(n_in);
  // A computed variable carries bash's att_noassign: the assignment is
  // silently discarded (and does not fail).  `BASH_ARGC=x' reaches here rather
  // than Shell::set, because an existing array takes the element-0 path.
  if (noassign_var(n)) return;
  // BASH_ALIASES is the live alias table: BASH_ALIASES[name]=value defines an
  // alias, after the same name validation the alias builtin performs.
  if (n == "BASH_ALIASES" && !is_zsh()) {
    if (!valid_alias_name(sub)) {
      std::fprintf(stderr, "%s`%s': invalid alias name\n", err_prefix().c_str(), sub.c_str());
      return;
    }
    alias_remember(sub, val);
    return;
  }
  // BASH_CMDS is the live command hash: BASH_CMDS[name]=value adds a hash
  // entry.  A `/' value is rejected in a restricted shell; a value without a
  // `/' is resolved through $PATH (and must be found) before it is stored.
  if (n == "BASH_CMDS") {
    if (val.find('/') != std::string::npos) {
      if (opt_restricted) {
        std::fprintf(stderr, "%s%s: restricted\n", err_prefix().c_str(), val.c_str());
        return;
      }
      hash_remember(sub, val);
      return;
    }
    std::string path = get("PATH"), full;
    size_t p = 0;
    while (p <= path.size()) {
      size_t q = path.find(':', p);
      std::string dir = path.substr(p, q == std::string::npos ? std::string::npos : q - p);
      if (dir.empty()) dir = ".";
      std::string cand = dir + "/" + val;
      if (access(cand.c_str(), X_OK) == 0) { full = cand; break; }
      if (q == std::string::npos) break;
      p = q + 1;
    }
    if (full.empty()) {
      std::fprintf(stderr, "%s%s: not found\n", err_prefix().c_str(), val.c_str());
      return;
    }
    hash_remember(sub, full);
    return;
  }
  // DIRSTACK is the live directory stack: DIRSTACK[N]=dir rewrites a stack entry
  // (index 0 is $PWD, kept implicit; 1.. map to dir_stack[N-1]).  Assigning it
  // must update the real stack, not create a shadow array that reads ignore.
  if (n == "DIRSTACK" && !is_zsh()) {
    bool ok = true;
    long long k = eval_arith(*this, sub, &ok);
    if (ok && k >= 1 && k - 1 < static_cast<long long>(dir_stack.size()))
      dir_stack[k - 1] = val;
    return;
  }
  Variable &v = vars[n];
  // `declare -u'/`-l'/`-c' fold the case of each element value on assignment,
  // exactly as Shell::set does for scalars.
  std::string fv = fold_case(val, v.ucase, v.lcase, v.capcase);
  if (v.kind == VarKind::Assoc) {
    if (v.readonly) {
      std::fprintf(stderr, "%s%s: readonly variable\n", err_prefix().c_str(), n.c_str());
      last_status = 1;
      return;
    }
    v.invisible = false;  // an assignment makes a declared-but-unset array visible
    assoc_put(v, sub, fv);
    return;
  }
  // The subscript is validated BEFORE the readonly attribute is consulted:
  // `readonly -a c; c[-2]=4' reports the bad subscript, not the readonly
  // violation (bash), and the error unwinds the current command list.
  bool ok = true;
  long long k = eval_arith(*this, sub, &ok);
  if (!ok) {
    // A malformed subscript reports bash's arithmetic diagnostic (naming the
    // subscript text), assigns nothing, and unwinds the current command list
    // (bash longjmps to the command loop; the enclosing subshell dies, but
    // the reader continues with the next line).
    eval_arith_msg(*this, sub, "", &ok);
    arith_abort = true;
    last_status = 1;
    return;
  }
  // A negative index counts back from the highest set index; one that resolves
  // below zero is a bad subscript (bash errors, leaves the array unchanged, and
  // unwinds the command list).  Under zsh the subscript was already translated.
  if (k < 0 && !is_zsh()) {
    // A set scalar counts as its would-be element 0 (`a=abcde; a[-1]=z').
    long long maxi = !v.idx.empty() ? v.idx.rbegin()->first
                    : (v.kind == VarKind::Scalar && !v.value.empty()) ? 0
                                                                      : -1;
    k += maxi + 1;
    if (k < 0) {
      std::fprintf(stderr, "%s%s[%s]: bad array subscript\n", err_prefix().c_str(),
                   n.c_str(), sub.c_str());
      arith_abort = true;
      last_status = 1;
      return;
    }
  }
  if (v.readonly) {
    std::fprintf(stderr, "%s%s: readonly variable\n", err_prefix().c_str(), n.c_str());
    last_status = 1;
    return;
  }
  v.invisible = false;  // an assignment makes a declared-but-unset array visible
  // A subscripted assignment to an existing scalar promotes it to an indexed
  // array, keeping the old value as element 0 (`a=abcde; a[2]=x' yields
  // ([0]=abcde [2]=x)), matching bash.
  if (v.kind == VarKind::Scalar && !v.value.empty() && v.idx.empty()) v.idx[0] = v.value;
  v.kind = VarKind::Indexed;
  v.idx[k] = fv;
  if (k == 0) v.value = fv;
}

bool Shell::array_unset(const std::string &n_in, const std::string &sub) {
  std::string n = deref(n_in);
  auto it = vars.find(n);
  if (it == vars.end()) return true;  // element of a missing array: no-op
  Variable &v = it->second;
  if (v.readonly) return true;
  // For an INDEXED array, `unset a[@]' / `unset a[*]' clears every element but
  // leaves the (now empty) array in place.  For an ASSOCIATIVE array, `@'/`*'
  // are ordinary literal keys (bash does not treat them as "all elements"), so
  // fall through and erase just that key.
  if ((sub == "@" || sub == "*") && v.kind != VarKind::Assoc) {
    v.idx.clear();
    v.assoc.clear();
    v.assoc_seq.clear();
    v.value.clear();
    return true;
  }
  if (v.kind == VarKind::Assoc) {
    v.assoc.erase(sub);
    v.assoc_seq.erase(std::remove(v.assoc_seq.begin(), v.assoc_seq.end(), sub),
                      v.assoc_seq.end());
    return true;
  }
  bool ok = true;
  long long k = eval_arith(*this, sub, &ok);
  if (!ok) return true;
  // A negative index counts back from the highest assigned index (`a[-1]').
  if (k < 0 && v.kind == VarKind::Indexed && !v.idx.empty())
    k += v.idx.rbegin()->first + 1;
  if (k < 0) return false;  // still out of range: bash reports a bad subscript
  if (v.kind == VarKind::Indexed) {
    v.idx.erase(k);
    if (k == 0) v.value.clear();  // element 0 is mirrored in the scalar field
  } else if (v.kind == VarKind::Scalar && k == 0) {
    vars.erase(it);  // a scalar's only element is itself
  }
  return true;
}

bool Shell::array_expand_once_ok(const std::string &base, std::string &sub) {
  auto it = shopt_opts.find("array_expand_once");
  if (it == shopt_opts.end() || !it->second) return true;
  if (sub == "@" || sub == "*") return true;
  auto vit = vars.find(deref(base));
  if (vit != vars.end() && vit->second.kind == VarKind::Assoc) return true;
  bool ok = true;
  long long idx = eval_arith_msg(*this, sub, "", &ok);
  if (!ok) return false;  // eval_arith_msg has already printed the diagnostic
  sub = std::to_string(idx);
  return true;
}

bool Shell::is_array(const std::string &n_in) const {
  std::string n = deref(n_in);
  auto it = vars.find(n);
  return it != vars.end() &&
         (it->second.kind == VarKind::Indexed || it->second.kind == VarKind::Assoc);
}

std::string Shell::zsh_subscript(const std::string &name, const std::string &sub) const {
  if (!is_zsh()) return sub;
  std::string n = deref(name);
  auto it = vars.find(n);
  // Associative arrays are keyed by string, not position -- never translate.
  if (it != vars.end() && it->second.kind == VarKind::Assoc) return sub;
  bool ok = true;
  long long k = eval_arith(const_cast<Shell &>(*this), sub, &ok);
  if (!ok) return sub;              // non-numeric: leave as-is
  if (k > 0) return std::to_string(k - 1);            // 1-based -> 0-based
  if (k < 0) return std::to_string(k + array_count(name));  // -1 == last
  return "-1";                     // zsh has no element 0: force a miss
}

int Shell::array_count(const std::string &n_in) const {
  {
    std::vector<std::pair<std::string, std::string>> vp;
    if (virtual_array(n_in, vp)) return static_cast<int>(vp.size());
  }
  std::string n = deref(n_in);
  auto it = vars.find(n);
  if (it == vars.end()) return 0;
  const Variable &v = it->second;
  if (v.kind == VarKind::Indexed) return static_cast<int>(v.idx.size());
  if (v.kind == VarKind::Assoc) return static_cast<int>(v.assoc.size());
  return 1;
}

void Shell::make_array(const std::string &n_in, bool assoc) {
  std::string n = deref(n_in);
  // A nameref whose target is an array element (`declare -n ref=a[0]; declare -A
  // ref') derefs to `a[0]'.  There is no variable named `a[0]'; bash applies the
  // array attribute to the base array `a' (`declare -A a'), so strip the
  // subscript here rather than creating a literal `a[0]' variable.
  if (size_t lb = n.find('['); lb != std::string::npos) n = n.substr(0, lb);
  bool fresh = !vars.count(n);
  Variable &v = vars[n];
  if (fresh) v.invisible = true;  // `declare -a b' with no value: declared, unset
  // `declare -a' on an unset nameref (deref lands on the nameref itself)
  // converts it into a genuine, still-unset array, dropping the reference.
  if (v.nameref) {
    v.nameref = false;
    v.invisible = true;
  }
  if (v.kind == VarKind::Scalar) {
    // Converting an existing scalar to an indexed array keeps its value as
    // element 0 (`a=abcde; declare -a a' leaves ${a[0]} == abcde), matching
    // bash.  An associative array has no natural key for the old value, so bash
    // discards it there.
    if (!assoc && !v.value.empty()) {
      v.idx[0] = v.value;
      v.value.clear();
    }
    // Converting to an associative array keeps the old value under the string
    // key "0" (bash convert_var_to_assoc), and the converted table simulates
    // bash's 128-bucket layout (assoc_create(0)) -- only a fresh `declare -A'
    // gets the 1024-bucket ASSOC_HASH_BUCKETS one, so the same keys can
    // enumerate differently (varenv11.sub).
    if (assoc && !v.value.empty()) {
      v.assoc_seq.push_back("0");
      v.assoc["0"] = v.value;
      v.value.clear();
    }
    // Only a SET (visible) scalar is a real conversion; a declared-but-unset
    // placeholder (a fresh `local'/`declare' cell) makes a NEW table with the
    // full 1024 buckets, exactly like bash's make_new_assoc_variable.
    if (assoc && !fresh && !v.invisible) v.assoc_buckets = 128;
    v.kind = assoc ? VarKind::Assoc : VarKind::Indexed;
  }
}

void Shell::array_assign(
    const std::string &n_in,
    const std::vector<std::pair<std::optional<std::string>, std::string>> &elems,
    bool append, bool assoc) {
  std::string n = deref(n_in);
  // A compound assignment through a nameref that points at an array element
  // (`declare -n ref=XXX[0]; ref+=(...)') has no valid whole-array target: bash
  // rejects the resolved `XXX[0]' as not a valid identifier and assigns nothing.
  if (n.find('[') != std::string::npos) {
    std::fprintf(stderr, "%s`%s': not a valid identifier\n", err_prefix().c_str(),
                 n.c_str());
    return;
  }
  bool existed = vars.count(n) != 0;
  Variable &v = vars[n];
  if (v.readonly) {
    // A compound assignment to a readonly variable reports and assigns
    // nothing (`readonly a=7; a=(1 2 3)' -- varenv11.sub).
    std::fprintf(stderr, "%s%s: readonly variable\n", err_prefix().c_str(), n.c_str());
    last_status = 1;
    return;
  }
  // Array-assigning to an unset nameref (deref stopped on the nameref itself)
  // converts it into a real array and drops the nameref attribute, with bash's
  // warning (assign_array_var_from_word_list -> make_variable_value).  A nameref
  // with a valid target resolves to that target above, so this never fires for
  // it.
  if (v.nameref) {
    std::fprintf(stderr, "%swarning: %s: removing nameref attribute\n",
                 err_prefix().c_str(), n.c_str());
    v.nameref = false;
  }
  v.invisible = false;  // even `b=()' makes a declared-but-unset array visible
  // `declare -l'/`-u'/`-c' fold the case of each ELEMENT VALUE (never a key),
  // exactly as Shell::set / array_set do for scalar and single-element writes.
  auto fold_val = [&v](std::string s) {
    return fold_case(std::move(s), v.ucase, v.lcase, v.capcase);
  };
  if (!append) {
    v.idx.clear();
    v.assoc.clear();
    v.assoc_seq.clear();
    v.value.clear();
  } else if (v.kind == VarKind::Scalar && existed && !v.invisible && !assoc) {
    // Appending a compound to a SET scalar converts it keeping the old value
    // as element 0: `s=X; s+=(Y)' -> ([0]="X" [1]="Y"), including the empty
    // string (bash); an unset variable starts at [0] with the new elements.
    v.idx[0] = v.value;
    v.value.clear();
  }
  v.kind = (assoc || v.kind == VarKind::Assoc) ? VarKind::Assoc : VarKind::Indexed;
  long long next = 0;
  if (v.kind == VarKind::Indexed && !v.idx.empty()) next = v.idx.rbegin()->first + 1;
  // An associative array can be assigned from a flat key/value list
  // (`declare -A h; h=(k1 v1 k2 v2)'), as bash 5.x and zsh both do, in addition
  // to explicit `([k]=v)' pairs (which keep their subscript below).
  if (v.kind == VarKind::Assoc) {
    for (size_t x = 0; x < elems.size(); x++) {
      if (elems[x].first) { assoc_put(v, *elems[x].first, fold_val(elems[x].second)); continue; }
      const std::string &key = elems[x].second;
      // An empty key in the flat list is a bad subscript; bash reports it as
      // `"": bad array subscript' and stops the assignment (assoc11.sub).
      if (key.empty()) {
        std::fprintf(stderr, "%s\"\": bad array subscript\n", err_prefix().c_str());
        last_status = 1;
        return;
      }
      assoc_put(v, key,
                fold_val((x + 1 < elems.size()) ? elems[x + 1].second : std::string()));
      x++;  // consumed the paired value
    }
    return;
  }
  for (const auto &e : elems) {
    if (v.kind == VarKind::Assoc) {
      if (e.first) assoc_put(v, *e.first, fold_val(e.second));
    } else if (e.first) {
      bool ok = true;
      long long k = eval_arith(*this, *e.first, &ok);
      if (!ok) k = 0;
      v.idx[k] = fold_val(e.second);
      next = k + 1;
    } else {
      v.idx[next++] = fold_val(e.second);
    }
  }
  if (v.kind == VarKind::Indexed && v.idx.count(0)) v.value = v.idx[0];
}

// ---- BASH_SOURCE / FUNCNAME / BASH_LINENO --------------------------------

// Rebuild the three call-context arrays from src_frames, matching bash: index 0
// is the innermost frame.  BASH_SOURCE lists the source file of every frame
// (including the base script).  FUNCNAME / BASH_LINENO are populated only while
// the innermost frame is a function -- then they run from that function down
// through the enclosing frames to a trailing "main" / 0 (the base script).
void Shell::sync_source_arrays() {
  auto set_indexed = [&](const char *name, const std::vector<std::string> &vals) {
    if (vals.empty()) { unset(name); return; }
    std::vector<std::pair<std::optional<std::string>, std::string>> e;
    e.reserve(vals.size());
    for (const auto &v : vals) e.push_back({std::nullopt, v});
    bool was_ro = vars.count(name) && vars[name].readonly;
    if (was_ro) vars[name].readonly = false;
    array_assign(name, e, false, false);
    if (was_ro) vars[name].readonly = true;
  };
  if (src_frames.empty()) {
    unset("BASH_SOURCE"); unset("FUNCNAME"); unset("BASH_LINENO");
    return;
  }
  std::vector<std::string> sources;
  for (auto it = src_frames.rbegin(); it != src_frames.rend(); ++it)
    sources.push_back(it->source);
  set_indexed("BASH_SOURCE", sources);

  if (!src_frames.back().is_func) {
    // At the base (script) frame bash still exposes BASH_LINENO=([0]="0") and an
    // invisible FUNCNAME (a declared array with no value).
    set_indexed("BASH_LINENO", {"0"});
    Variable &fn = vars["FUNCNAME"];
    fn.kind = VarKind::Indexed;
    fn.idx.clear();
    fn.assoc.clear();
    fn.assoc_seq.clear();
    fn.value.clear();
    fn.invisible = true;
    return;
  }
  // A script run from a file (or sourced) has a base "main" frame at index 0
  // that FUNCNAME/BASH_LINENO trail with `main'/`0'; `sh -c' has no such frame,
  // so its FUNCNAME/BASH_LINENO list only the active function frames.
  bool has_base = !src_frames.front().is_func;
  size_t stop = has_base ? 1 : 0;
  std::vector<std::string> names, lines;
  for (size_t i = src_frames.size(); i-- > stop;) {  // top down to the first frame
    names.push_back(src_frames[i].name);
    lines.push_back(std::to_string(src_frames[i].line));
  }
  if (has_base) {
    names.push_back("main");
    lines.push_back("0");
  }
  set_indexed("FUNCNAME", names);
  set_indexed("BASH_LINENO", lines);
}

void Shell::push_src_frame(const std::string &name, const std::string &source, int line,
                           bool is_func) {
  src_frames.push_back({name, source, line, is_func});
  sync_source_arrays();
}

void Shell::pop_src_frame() {
  if (!src_frames.empty()) src_frames.pop_back();
  sync_source_arrays();
}

// ---- local scopes ---------------------------------------------------------

void Shell::push_scope() {
  local_stack.emplace_back();
  getopt_scope_saves.emplace_back();  // no OPTIND localized here yet
}

void Shell::pop_scope() {
  if (local_stack.empty()) return;
  auto &scope = local_stack.back();
  for (auto it = scope.rbegin(); it != scope.rend(); ++it) {
    // `local -': the entry holds a set -o option snapshot, not a variable.
    if (it->first == "-" && it->second &&
        it->second->value.compare(0, 9, "\x01SETOPTS:") == 0) {
      const std::string &sv = it->second->value;
      size_t p = 9;
      while (p < sv.size()) {
        size_t eq = sv.find('=', p), sc = sv.find(';', p);
        if (eq == std::string::npos || sc == std::string::npos) break;
        if (sv.compare(p, eq - p, "restricted") != 0)  // cannot be cleared
          apply_set_o_option(*this, sv.substr(p, eq - p), sv[eq + 1] == '1');
        p = sc + 1;
      }
      continue;
    }
    if (it->second)
      vars[it->first] = *it->second;
    else
      vars.erase(it->first);
  }
  local_stack.pop_back();
  if (!getopt_scope_saves.empty()) {
    if (getopt_scope_saves.back()) {  // this scope had `local OPTIND'
      auto &g = *getopt_scope_saves.back();
      getopt_charidx = std::get<0>(g);
      getopt_curarg = std::get<1>(g);
      getopt_optind = std::get<2>(g);
    }
    getopt_scope_saves.pop_back();
  }
}

bool Shell::make_local(const std::string &n, bool inherit_force) {
  if (local_stack.empty()) return false;  // `local' outside a function: no-op scope
  auto &scope = local_stack.back();
  for (auto &e : scope)
    if (e.first == n) return false;  // already made local in this scope
  auto it = vars.find(n);
  // Under an active temp env (`v=t declare -x v'), the new local CONSUMES the
  // temp layer: its frame-restore point is the PRE-temp binding (so the
  // caller's v is restored at return), and the command's temp undo skips it.
  auto tp = temp_prior.find(n);
  if (tp != temp_prior.end() && temp_env_active.count(n)) {
    scope.emplace_back(n, tp->second);
    temp_consumed.insert(n);
    temp_prior.erase(tp);
  } else {
    scope.emplace_back(n, it == vars.end() ? std::nullopt : std::optional<Variable>(it->second));
  }
  // A fresh local inherits the value and attributes of the nearest enclosing
  // variable of the same name rather than starting unset when `-I' was given or
  // `shopt -s localvar_inherit' is set (bash); a later `=value' on the local
  // overrides the value.  The enclosing binding is still live in `vars[n]', so
  // inheriting just means leaving it in place.  (A readonly enclosing global is
  // rejected before we reach here, so an inherited copy is always assignable.)
  auto liv = shopt_opts.find("localvar_inherit");
  bool shopt_on = liv != shopt_opts.end() && liv->second;
  // A variable passed in the temporary environment (`v=t f') is inherited by a
  // `local'/`typeset' of the same name inside the called function -- value and
  // (exported) attributes -- unconditionally, like `-I'/localvar_inherit do.
  bool inherit = it != vars.end() &&
                 (inherit_force || shopt_on || temp_env_active.count(n) != 0);
  if (!inherit) {
    // A local of an exported variable stays exported even without inheriting
    // its value, so the environment the function passes to child processes is
    // unchanged (bash): `export V; f(){ local V; }' keeps V in the environment
    // WITH the enclosing value (environ_block falls through the invisible
    // local to the shadowed binding), and a temp-env `V=x f' makes the local
    // `V' exported too.
    bool was_exported = it != vars.end() && it->second.exported;
    vars[n] = Variable{};  // fresh (unset) local
    vars[n].invisible = true;
    vars[n].exported = was_exported;
  }
  // Localizing OPTIND saves and resets the getopts scan state (bash restores
  // it when the function returns).
  if (n == "OPTIND" && !getopt_scope_saves.empty() && !getopt_scope_saves.back()) {
    getopt_scope_saves.back() = std::make_tuple(getopt_charidx, getopt_curarg, getopt_optind);
    getopt_charidx = 1;
    getopt_curarg.clear();
    getopt_optind = 0;
  }
  return inherit;
}

bool Shell::get_if_set(const std::string &n_in, std::string &out) const {
  std::string base, sub;
  if (nameref_elt(n_in, base, sub)) {
    if (!is_set(n_in)) return false;
    out = array_get(base, sub);
    return true;
  }
  bool circular = false, too_deep = false;
  std::string n = deref_ex(n_in, circular, &too_deep);
  // A chain longer than nameref_max() resolves to nothing, with bash's warning
  // naming the limit actually in force.
  if (too_deep) {
    std::fprintf(stderr, "%swarning: %s: maximum nameref depth (%d) exceeded\n",
                 err_prefix().c_str(), n_in.c_str(), nameref_max());
    return false;
  }
  if (circular) {
    // Same resolution as get(): warn, and at function scope fall through to the
    // global binding; a global-scope cycle resolves to nothing (unset).
    std::fprintf(stderr, "%swarning: %s: circular name reference\n",
                 err_prefix().c_str(), n_in.c_str());
    const Variable *g = in_function() ? global_var_ptr(n_in) : nullptr;
    if (!g) return false;
    if (g->kind == VarKind::Indexed) {
      if (!g->idx.count(0)) return false;
      out = g->idx.at(0);
      return true;
    }
    if (g->kind == VarKind::Assoc) {
      if (!g->assoc.count("0")) return false;
      out = g->assoc.at("0");
      return true;
    }
    out = g->value;
    return true;
  }
  auto it = vars.find(n);
  if (it == vars.end()) return false;
  // A nameref with no (or a self) target -- deref stops on it -- is unset.
  if (it->second.nameref) return false;
  const Variable &v = it->second;
  // A declared-but-unassigned (invisible) variable is unset: `declare x' / `local
  // x' make an att_invisible variable that ${x-default}/${x+...}/-v treat as unset
  // until it is assigned.
  if (v.invisible) return false;
  // An array in scalar context is its element 0 (indexed) / "0" (assoc).
  if (v.kind == VarKind::Indexed) {
    if (!v.idx.count(0)) return false;
    out = v.idx.at(0);
    return true;
  }
  if (v.kind == VarKind::Assoc) {
    if (!v.assoc.count("0")) return false;
    out = v.assoc.at("0");
    return true;
  }
  out = v.value;
  return true;
}

static bool is_locale_var(const std::string &n) {
  return n == "LC_ALL" || n == "LC_CTYPE" || n == "LANG";
}

// Re-apply the LC_CTYPE locale from the shell's own LC_ALL/LC_CTYPE/LANG
// variables, in POSIX precedence order, so a runtime `export LC_ALL=en_US.UTF-8'
// makes subsequent ${#var}/substring operations count characters.  Only the
// LC_CTYPE category (which governs MB_CUR_MAX / mbrtowc) is changed.
static void apply_ctype_locale(Shell &sh) {
  auto val = [&](const char *k) -> std::string {
    auto it = sh.vars.find(k);
    return it != sh.vars.end() ? it->second.value : std::string();
  };
  std::string loc = val("LC_ALL");
  if (loc.empty()) loc = val("LC_CTYPE");
  if (loc.empty()) loc = val("LANG");
  if (!loc.empty()) std::setlocale(LC_CTYPE, loc.c_str());
}

bool Shell::set(const std::string &n_in, const std::string &v,
                const char *nameref_ctx) {
  // A DIRECT element reference (`A[sub]', e.g. a `read A[k]' target) writes
  // through to the array element -- previously this silently created a shadow
  // variable literally named "A[sub]".  The subscript spans to the LAST
  // bracket, so a lenient assoc_expand_once target (`A[]]') keys on `]'.
  {
    size_t lb = n_in.find('[');
    if (lb != std::string::npos && lb > 0 && n_in.back() == ']') {
      std::string base = n_in.substr(0, lb);
      std::string sub = n_in.substr(lb + 1, n_in.size() - lb - 2);
      auto bit = vars.find(deref(base));
      if (bit != vars.end() && bit->second.readonly) {
        std::fprintf(stderr, "%s%s: readonly variable\n", err_prefix().c_str(),
                     base.c_str());
        return false;
      }
      array_set(base, sub, v);
      return true;
    }
  }
  // A nameref whose target is an array element (`declare -n r=a[2]'): write
  // through to that element.  array_set enforces the target's readonly flag.
  {
    std::string base, sub;
    if (nameref_elt(n_in, base, sub)) {
      // A nameref whose OWN value names an element of itself (`local -n a=a[0];
      // a=X') is circular; after the declaration's circular-reference warnings
      // bash rejects the write as `a[0]: not a valid identifier' and assigns
      // nothing.  Test the immediate value, not the resolved base: an indirect
      // chain (`a->b; b=a[1]; a=foo') is handled elsewhere by removing a's
      // nameref attribute, not by this error.
      {
        auto nv = vars.find(n_in);
        if (nv != vars.end() && nv->second.nameref) {
          const std::string &ov = nv->second.value;
          size_t lb = ov.find('[');
          if (lb != std::string::npos && lb > 0 && ov.back() == ']' &&
              ov.compare(0, lb, n_in) == 0) {
            std::fprintf(stderr, "%s`%s[%s]': not a valid identifier\n",
                         err_prefix().c_str(), base.c_str(), sub.c_str());
            return false;
          }
        }
      }
      // A scalar assignment through a nameref to a whole-array splat
      // (`declare -n r=a[@]; r=5') has no single element to target: bash rejects
      // the `@'/`*' subscript as a bad array subscript and assigns nothing.
      if (sub == "@" || sub == "*") {
        std::fprintf(stderr, "%s%s[%s]: bad array subscript\n", err_prefix().c_str(),
                     base.c_str(), sub.c_str());
        return false;
      }
      // The chain looped back to an ELEMENT of a variable that is itself a
      // nameref (`typeset -n a=b b; b=a[1]; a=foo'): bash cannot both keep `a'
      // a reference and subscript it, so it drops the nameref attribute with a
      // warning and makes the variable an array (nameref15.sub).
      {
        auto bv = vars.find(base);
        if (bv != vars.end() && bv->second.nameref) {
          std::fprintf(stderr, "%swarning: %s: removing nameref attribute\n",
                       err_prefix().c_str(), base.c_str());
          bv->second.nameref = false;
          bv->second.value.clear();
          bv->second.invisible = false;
        }
      }
      auto it = vars.find(base);
      if (it != vars.end() && it->second.readonly) {
        std::fprintf(stderr, "%s%s: readonly variable\n", err_prefix().c_str(),
                     base.c_str());
        return false;
      }
      array_set(base, sub, v);
      return true;
    }
  }
  bool circular = false;
  bool too_deep = false;
  std::string n = deref_ex(n_in, circular, &too_deep);
  // Too long a chain has no variable at the end to assign to: bash warns and
  // fails the assignment, which (with no command word) abandons the list.
  if (too_deep) {
    std::fprintf(stderr, "%swarning: %s: maximum nameref depth (%d) exceeded\n",
                 err_prefix().c_str(), n_in.c_str(), nameref_max());
    return false;
  }
  // A circular nameref (self reference `v->v' or a longer loop).  At function
  // scope bash reports the loop as exceeding the max nameref depth and binds
  // the value at global scope; at global scope it warns and the assignment has
  // no persistent effect.
  if (circular) {
    if (!in_function()) {
      std::fprintf(stderr, "%swarning: %s: circular name reference\n",
                   err_prefix().c_str(), n_in.c_str());
      // A cycle has nothing to assign to, so this is an assignment ERROR:
      // `x=4; echo A' warns and abandons the rest of the list.  Inside a
      // function the reference binds at global scope instead and execution
      // carries on, which is why only the global-scope branch fails.
      return false;
    }
    std::fprintf(stderr, "%swarning: %s: maximum nameref depth (8) exceeded\n",
                 err_prefix().c_str(), n_in.c_str());
    Variable &g = global_var_ref(n_in);
    if (g.readonly) {
      std::fprintf(stderr, "%s%s: readonly variable\n", err_prefix().c_str(),
                   n_in.c_str());
      return false;
    }
    if (g.kind == VarKind::Indexed) {
      g.idx[0] = v;
    } else if (g.kind == VarKind::Assoc) {
      if (!g.assoc.count("0")) g.assoc_seq.push_back("0");
      g.assoc["0"] = v;
    } else {
      g.value = v;
    }
    return true;
  }
  // Assigning to a nameref that has no target yet sets its target (`declare -n
  // r; r=x' points r at x); the value must be a valid identifier.  deref stops
  // on such a nameref, so n still names it.  bash rejects a non-identifier here.
  {
    auto it = vars.find(n);
    // An empty value is rejected too: setting a targetless nameref's target to
    // "" (`declare -n r; r=""') is an invalid identifier in bash, unlike a plain
    // scalar `x=""'.
    if (it != vars.end() && it->second.nameref && it->second.value.empty() &&
        !valid_nameref_target(v)) {
      bool has_ctx = nameref_ctx && nameref_ctx[0];
      std::fprintf(stderr, "%s%s%s`%s': not a valid identifier\n", err_prefix().c_str(),
                   has_ctx ? nameref_ctx : "", has_ctx ? ": " : "", v.c_str());
      return false;
    }
  }
  // Assigning BASH_ARGV0 resets $0 but NOT the name shown in error messages:
  // bash reports errors against the source file, not the mutable $0.  $0 already
  // resolves through arg0; still stored so `$BASH_ARGV0' reads back the value.
  if (n == "BASH_ARGV0") { arg0 = v; }
  // Assigning to a dynamic variable seeds/rebases it rather than storing.
  if (n == "RANDOM") {
    rand_seed = static_cast<unsigned long>(std::strtoul(v.c_str(), nullptr, 10));
    rand_seeded = true;
    return true;
  }
  if (n == "SECONDS") {
    seconds_base = static_cast<long long>(std::time(nullptr)) -
                   std::strtoll(v.c_str(), nullptr, 10);
    return true;
  }
  if (opt_restricted &&
      (n == "PATH" || n == "SHELL" || n == "ENV" || n == "BASH_ENV")) {
    std::fprintf(stderr, "%s%s: readonly variable\n", err_prefix().c_str(), n.c_str());
    return false;
  }
  // A computed variable carries bash's att_noassign: a scalar assignment is
  // silently discarded without error, so `LINENO=999' succeeds and changes
  // nothing.
  if (noassign_var(n)) return true;
  // $SHELLOPTS and $BASHOPTS are readonly instead: assigning to one is an
  // error, which (having no command word) also abandons the command list.
  if (!is_zsh() && (n == "SHELLOPTS" || n == "BASHOPTS")) {
    std::fprintf(stderr, "%s%s: readonly variable\n", err_prefix().c_str(), n.c_str());
    return false;
  }
  Variable &var = vars[n];
  if (var.readonly) {
    std::fprintf(stderr, "%s%s: readonly variable\n", err_prefix().c_str(), n.c_str());
    return false;
  }
  var.value = v;
  var.invisible = false;  // an assignment makes a declared-but-unset scalar visible
  // `set -a': every assignment marks the variable for export (bash allexport).
  if (opt_allexport) var.exported = true;
  // Setting POSIXLY_CORRECT (to any value) enables POSIX mode, as in bash.
  if (n == "POSIXLY_CORRECT") opt_posix = true;
  // BASHLY_CORRECT is a switch rather than a flag: only the exact value `true'
  // turns it on, and anything else turns it back off.
  if (n == "BASHLY_CORRECT") apply_bashly_correct(v == "true");
  // A locale assignment re-applies LC_CTYPE so multibyte handling follows it.
  if (is_locale_var(n)) apply_ctype_locale(*this);
  // `declare -u' / `-l' / `-c' fold the value's case on every assignment.
  var.value = fold_case(var.value, var.ucase, var.lcase, var.capcase);
  // A scalar assignment to an ARRAY variable writes element 0 (`declare -a x;
  // read x' stores x[0]; `declare -A x; x=v' keys "0"), as bash does.
  if (var.kind == VarKind::Indexed) var.idx[0] = var.value;
  else if (var.kind == VarKind::Assoc) {
    if (!var.assoc.count("0")) var.assoc_seq.push_back("0");
    var.assoc["0"] = var.value;
  }
  // Assigning HISTSIZE re-stifles the loaded history list, as bash does; a
  // non-numeric or empty value leaves the list unbounded.
  if (n == "HISTSIZE" && history_loaded) {
    char *e = nullptr;
    long hv = std::strtol(v.c_str(), &e, 10);
    if (!v.empty() && e && *e == '\0' && hv >= 0) stifle_history(static_cast<int>(hv));
    else unstifle_history();
  }
  return true;
}

void Shell::set_exported(const std::string &n_in, const std::string &v) {
  std::string n = deref(n_in);
  Variable &var = vars[n];
  if (var.readonly) return;
  var.value = v;
  var.exported = true;
  var.invisible = false;  // an assignment makes a declared-but-unset scalar visible
  if (n == "POSIXLY_CORRECT") opt_posix = true;
  if (n == "BASHLY_CORRECT") apply_bashly_correct(v == "true");
  if (is_locale_var(n)) apply_ctype_locale(*this);
}

void Shell::export_name(const std::string &n) {
  // `export NAME' with no value marks NAME for export but does not give it a
  // value: a previously-unset variable stays unset (bash's att_invisible), so
  // `${NAME+set}' is empty and `declare -p NAME' prints just `declare -x NAME'
  // (no `=').  An existing variable keeps its value and visibility.
  // `export ref' follows a nameref and exports its TARGET, leaving the nameref
  // itself unexported (bash's declare_internal resolves the name first).
  std::string t = deref(n);
  bool fresh = vars.find(t) == vars.end();
  Variable &var = vars[t];
  var.exported = true;
  if (fresh) var.invisible = true;
}

void Shell::unset(const std::string &n_in, bool force, bool noref) {
  if (n_in == "HISTSIZE" && history_loaded) unstifle_history();
  // `unset name' on a nameref removes the target; `unset -n name' (NOREF)
  // removes the nameref variable itself.  Following the ref matches common
  // usage.  FORCE mirrors bash's unbind_variable_noref: remove the named
  // variable itself (no nameref following) even when it is readonly.
  std::string n = (force || noref) ? n_in : deref(n_in);
  // A nameref aimed at an array ELEMENT unsets that element, not a variable
  // literally named `v[1]' (`declare -n n=v[1]; unset n' -- nameref15.sub).
  if (n != n_in && n.size() > 2 && n.back() == ']') {
    size_t lb = n.find('[');
    if (lb != std::string::npos && lb > 0) {
      array_unset(n.substr(0, lb), n.substr(lb + 1, n.size() - lb - 2));
      return;
    }
  }
  if (n == "POSIXLY_CORRECT") opt_posix = false;  // unsetting leaves POSIX mode
  // Unsetting it is "any other value": the saved nameref limit comes back.
  if (n == "BASHLY_CORRECT") apply_bashly_correct(false);
  auto it = vars.find(n);
  if (it != vars.end() && (force || !it->second.readonly)) {
    // Unsetting a variable that is local to the CURRENT function scope leaves
    // the (now unset) local binding in place rather than removing it, so it
    // keeps shadowing any enclosing value and a later assignment reuses the
    // local -- `local v=x; unset v; declare -p v' still reports `declare -- v'
    // (bash).  A forced unset (getopts clearing OPTARG) and a non-local target
    // are removed outright.
    bool cur_local = false;
    if (!force && !local_stack.empty())
      for (auto &e : local_stack.back())
        if (e.first == n) { cur_local = true; break; }
    if (cur_local) {
      Variable v;
      v.invisible = true;
      // The declared-but-unset placeholder keeps the export attribute (a
      // temp-env-inherited local stays `declare -x v' after unset, bash).
      v.exported = it->second.exported;
      vars[n] = v;
      return;
    }
    // Unsetting a variable that is local to an ENCLOSING function scope POPS
    // that local cell (bash's value-stack semantics): the binding beneath it
    // -- an outer local or the global -- becomes visible immediately, at
    // every scope, and the popped frame entry must not restore it again at
    // return.  `outer(){ local r; inner; echo $r; }; inner(){ unset r; }'
    // exposes the global r inside BOTH inner and outer (varenv10.sub).
    if (!force && local_stack.size() > 1) {
      for (auto fit = std::next(local_stack.rbegin()); fit != local_stack.rend(); ++fit) {
        for (auto e = fit->begin(); e != fit->end(); ++e) {
          if (e->first != n) continue;
          // `shopt -s localvar_unset': treat the enclosing scope's local as
          // if unset THERE (ash semantics, varenv24.sub) -- it stays a local,
          // now-unset binding until its frame returns -- instead of popping
          // the cell to expose what lies beneath.
          auto lvu = shopt_opts.find("localvar_unset");
          if (lvu != shopt_opts.end() && lvu->second) {
            Variable v;
            v.invisible = true;
            v.exported = it->second.exported;
            vars[n] = v;
            return;
          }
          if (e->second) vars[n] = *e->second;
          else vars.erase(n);
          fit->erase(e);
          return;
        }
      }
    }
    vars.erase(it);
  }
}

std::string Shell::ifs() const {
  auto it = vars.find("IFS");
  return it == vars.end() ? std::string(" \t\n") : it->second.value;
}

std::vector<std::string> Shell::environ_block() const {
  std::vector<std::string> out;
  for (const auto &kv : vars) {
    std::string val = kv.second.value;
    bool emit = kv.second.exported;
    // An INVISIBLE local passes the SHADOWED exported value to children until
    // it is assigned (bash): `export V=abc; f(){ local V; }' keeps V=abc in
    // the environment, and so does `typeset +x V' (an unexported invisible
    // local does not block the exported global underneath -- varenv12.sub).
    if (kv.second.invisible) {
      bool found = false;
      for (auto fit = local_stack.rbegin(); fit != local_stack.rend() && !found; ++fit)
        for (const auto &e : *fit)
          if (e.first == kv.first) {
            if (e.second && !e.second->invisible && e.second->exported) {
              val = e.second->value;
              emit = true;
            } else {
              val.clear();
            }
            found = true;
            break;
          }
    }
    if (!emit) continue;
    out.push_back(kv.first + "=" + val);
  }
  // Exported functions travel as BASH_FUNC_<name>%%=() {  body  }, which a
  // child bash/gnash re-imports at startup.
  for (const auto &name : exported_functions) {
    auto it = functions.find(name);
    if (it == functions.end()) continue;
    std::string s = named_function_string(name, it->second);  // "name () \n{...}"
    size_t br = s.find('(');  // drop the leading "name " so the value is "() {...}"
    if (br == std::string::npos) continue;
    out.push_back("BASH_FUNC_" + name + "%%=" + s.substr(br));
  }
  return out;
}

void Shell::import_env_functions() {
  // Collect first, then mutate `vars' (we erase the BASH_FUNC_ entries).
  std::vector<std::pair<std::string, std::string>> found;
  for (const auto &kv : vars) {
    const std::string &k = kv.first;
    if (k.compare(0, 10, "BASH_FUNC_") == 0 && k.size() > 12 &&
        k.compare(k.size() - 2, 2, "%%") == 0)
      found.emplace_back(k.substr(10, k.size() - 12), kv.second.value);
  }
  for (const auto &f : found) {
    const std::string &name = f.first;
    // Parse `name value' (value is "() {...}").  Accept only a lone function
    // definition -- any trailing command (a Shellshock payload) is rejected.
    ParseResult r = parse(name + " " + f.second);
    if (r.ok && r.command && dynamic_cast<const FunctionDef *>(r.command.get())) {
      const auto *fd = static_cast<const FunctionDef *>(r.command.get());
      functions[fd->name] = fd->body.get();
      func_lineno_base[fd->name] = 0;
      retained.push_back(std::move(r.command));
    }
    vars.erase("BASH_FUNC_" + name + "%%");  // not a real environment variable
  }
}

void Shell::set_personality(const std::string &name) {
  // `strict-bash' is not a shell of its own: it is the bash personality with
  // $BASHLY_CORRECT turned on, so the shell identifies itself as bash and picks
  // up bash's startup files while the stricter limits apply.  Selecting any
  // other personality turns the switch back off.
  bool strict = name == "strict-bash";
  personality_name = strict ? "bash" : name;
  const std::string &nm = personality_name;
  if (nm == "zsh") persona = Persona::Zsh;
  else if (nm == "ash" || nm == "dash" || nm == "sh") persona = Persona::Ash;
  else if (nm == "ksh" || nm == "ksh93" || nm == "mksh" || nm == "pdksh" ||
           nm == "rksh")
    persona = Persona::Ksh;
  else if (nm == "csh" || nm == "tcsh") persona = Persona::Csh;
  else persona = Persona::Bash;
  set("GNASH_PERSONALITY", nm);
  // Only `strict-bash' touches the switch.  Switching to another personality
  // leaves it alone: it is an ordinary variable the user may have set on
  // purpose (or inherited from the environment), and assigning to it is how you
  // turn it off.
  if (strict) set("BASHLY_CORRECT", "true");

  // Per-shell identity variables.  These are additive (as zsh's emulate is):
  // switching does not unset another shell's version variable.
  std::string exec_path = get("SHELL");
  std::string mach = get("MACHTYPE");
  if (persona == Persona::Zsh) {
    set("ZSH_VERSION", "5.9");
    set("ZSH_NAME", "zsh");
  } else if (persona == Persona::Ksh) {
    set("KSH_VERSION", "Version AJM 93u+ 2012-08-01");
  } else if (persona == Persona::Ash) {
    // ash is minimal: it advertises no BASH_/ZSH_ identity variables.  A
    // shell invoked as `sh' runs with POSIX semantics, exactly as bash does
    // (expansion errors are fatal to a non-interactive shell, etc).
    opt_posix = true;
  } else if (persona == Persona::Csh) {
    set("shell", exec_path);
  } else {
    set("BASH", exec_path);
    set("BASH_VERSION", "5.3.0(1)-release");
    std::vector<std::pair<std::optional<std::string>, std::string>> vi = {
        {std::nullopt, "5"}, {std::nullopt, "3"},       {std::nullopt, "0"},
        {std::nullopt, "1"}, {std::nullopt, "release"}, {std::nullopt, mach}};
    if (vars.count("BASH_VERSINFO")) vars["BASH_VERSINFO"].readonly = false;
    array_assign("BASH_VERSINFO", vi, false, false);
    vars["BASH_VERSINFO"].readonly = true;
    if (!is_set("BASH_LOADABLES_PATH"))
      set("BASH_LOADABLES_PATH",
          "/usr/local/lib/bash:/usr/lib/bash:/opt/local/lib/bash:"
          "/usr/pkg/lib/bash:/opt/pkg/lib/bash:.");
    // bash always lists these as (empty) indexed arrays; their live values are
    // served dynamically by virtual_array for reads, so the stored array stays
    // empty and only surfaces in `declare'/`set' listings.
    for (const char *nm : {"BASH_ARGC", "BASH_ARGV", "DIRSTACK"})
      if (!vars.count(nm)) array_assign(nm, {}, false, false);
    // BASH_ALIASES / BASH_CMDS are the associative equivalents (empty stored
    // arrays; virtual_array serves the live alias/hash tables for reads).
    for (const char *nm : {"BASH_ALIASES", "BASH_CMDS"})
      if (!vars.count(nm)) array_assign(nm, {}, false, true);
    // GROUPS is the supplementary group list, with the REAL gid forced to
    // element 0: bash prepends it when getgroups() omits it and swaps it
    // forward when it is merely out of order (initialize_group_array).
    if (!vars.count("GROUPS")) {
      long maxg = sysconf(_SC_NGROUPS_MAX);
      if (maxg <= 0) maxg = NGROUPS_MAX;
      std::vector<gid_t> g(static_cast<size_t>(maxg));
      int ng = getgroups(static_cast<int>(maxg), g.data());
      gid_t rgid = getgid();
      if (ng <= 0) { g[0] = rgid; ng = 1; }
      g.resize(static_cast<size_t>(ng));
      auto at = std::find(g.begin(), g.end(), rgid);
      if (at == g.end()) g.insert(g.begin(), rgid);
      else std::iter_swap(g.begin(), at);
      std::vector<std::pair<std::optional<std::string>, std::string>> gv;
      for (gid_t id : g) gv.emplace_back(std::nullopt, std::to_string(id));
      array_assign("GROUPS", gv, false, false);
    }
  }

  // Let an interactive REPL re-apply persona-dependent readline hooks when the
  // personality is switched at runtime (`personality'/`emulate').
  if (on_personality_change) on_personality_change();
}

// ---- history wiring (bash's bashhist.c equivalents) ------------------------

namespace {

Shell *g_hist_shell = nullptr;  // for the inhibit callback below

// Port of bash's bash_history_inhibit_expansion(): the `!' at STRING[I] is
// not a history expansion -- glob negation, ${!var}, $!, extglob !(...), or
// shell-quoted (scanning with quote rules, including fresh quoting contexts
// inside $(...) and backquotes; posix mode treats double quotes as quoting
// the expansion character).
int gnash_history_inhibit_expansion(char *string, int i) {
  if (i > 0 && string[i - 1] == '[' && std::strchr(string + i + 1, ']')) return 1;
  if (i > 1 && string[i - 1] == '{' && string[i - 2] == '$' &&
      std::strchr(string + i + 1, '}'))
    return 1;
  if (i > 0 && string[i - 1] == '$') return 1;
  if (string[i + 1] == '(' && std::strchr(string + i + 2, ')')) return 1;

  bool posix = g_hist_shell && g_hist_shell->opt_posix;
  int dquote = history_quoting_state == '"';
  std::vector<int> saved_dq;  // dquote state saved at each $( / ` level
  bool in_backq = false;
  size_t p = 0;
  const size_t target = static_cast<size_t>(i);
  while (string[p] && p < target) {
    char c = string[p];
    if (c == '\\') {
      p += string[p + 1] ? 2 : 1;
      continue;
    }
    if (c == '\'' && !dquote) {  // skip the single-quoted section
      p++;
      while (string[p] && string[p] != '\'') p++;
      if (string[p]) p++;
      continue;
    }
    if (c == '"') {
      if (posix) {  // posix: double quotes quote the expansion char entirely
        p++;
        while (string[p] && string[p] != '"') {
          if (string[p] == '\\' && string[p + 1]) p++;
          p++;
        }
        if (string[p]) p++;
      } else {
        dquote = 1 - dquote;
        p++;
      }
      continue;
    }
    if (c == '$' && string[p + 1] == '(') {  // fresh quoting context
      saved_dq.push_back(dquote);
      dquote = 0;
      p += 2;
      continue;
    }
    if (c == ')' && !saved_dq.empty()) {
      dquote = saved_dq.back();
      saved_dq.pop_back();
      p++;
      continue;
    }
    if (c == '`') {
      if (in_backq && !saved_dq.empty()) {
        dquote = saved_dq.back();
        saved_dq.pop_back();
      } else {
        saved_dq.push_back(dquote);
        dquote = 0;
      }
      in_backq = !in_backq;
      p++;
      continue;
    }
    p++;
  }
  if (p != target) return 1;                    // quoted away: not an expansion
  if (dquote && string[i + 1] == '"') return 1; // `!"' inside double quotes
  return 0;
}

}  // namespace

void Shell::enable_history() {
  opt_history = true;
  if (history_loaded) return;
  history_loaded = true;
  using_history();
  history_multiline_entries = is_set("HISTTIMEFORMAT") ? 1 : 0;
  std::string hf = get("HISTFILE");
  if (!hf.empty()) read_history(hf.c_str());
  if (is_set("HISTSIZE")) {
    const std::string hs = get("HISTSIZE");
    char *e = nullptr;
    long v = std::strtol(hs.c_str(), &e, 10);
    if (!hs.empty() && e && *e == '\0' && v >= 0) stifle_history(static_cast<int>(v));
  }
  using_history();
}

void Shell::sync_histchars() {
  history_quotes_inhibit_expansion = 1;
  using_history();  // `!!' and relative events resolve from the end of the list
  // bash's no-expand set and inhibition callback (globs, ${!var}, quoting).
  history_no_expand_chars = const_cast<char *>(" \t\n\r=;&|()<>");
  history_inhibit_expansion_function = gnash_history_inhibit_expansion;
  g_hist_shell = this;
  std::string hc = get("histchars");
  history_expansion_char = hc.size() > 0 ? hc[0] : '!';
  history_subst_char = hc.size() > 1 ? hc[1] : '^';
  history_comment_char = hc.size() > 2 ? hc[2] : '#';
}

bool Shell::add_history_line(const std::string &line) {
  // $HISTCONTROL: ignorespace / ignoredups / ignoreboth (erasedups not modeled).
  std::string hc = get("HISTCONTROL");
  bool ign_space = hc.find("ignorespace") != std::string::npos ||
                   hc.find("ignoreboth") != std::string::npos;
  bool ign_dups = hc.find("ignoredups") != std::string::npos ||
                  hc.find("ignoreboth") != std::string::npos;
  if (ign_space && !line.empty() && (line[0] == ' ' || line[0] == '\t')) return false;

  const char *prev = nullptr;
  if (history_length > 0) {
    HIST_ENTRY *e = history_get(history_base + history_length - 1);
    if (e) prev = e->line;
  }
  if (ign_dups && prev && line == prev) return false;

  // $HISTIGNORE: colon-separated patterns; `&' matches the previous entry.
  std::string hi = get("HISTIGNORE");
  size_t p = 0;
  while (p <= hi.size() && !hi.empty()) {
    size_t q = hi.find(':', p);
    std::string pat = hi.substr(p, q == std::string::npos ? std::string::npos : q - p);
    if (pat == "&") {
      if (prev && line == prev) return false;
    } else if (!pat.empty() &&
               strmatch(const_cast<char *>(pat.c_str()), const_cast<char *>(line.c_str()),
                        FNM_EXTMATCH) == 0) {
      return false;
    }
    if (q == std::string::npos) break;
    p = q + 1;
  }

  add_history(line.c_str());
  hist_new_entries++;
  return true;
}

void Shell::append_history_line(const std::string &line, bool heredoc) {
  if (history_length == 0) return;
  HIST_ENTRY *e = history_get(history_base + history_length - 1);
  if (e == nullptr || e->line == nullptr) return;
  std::string cur = e->line;

  if (heredoc) {  // keep the document's line structure
    std::string joined = cur + "\n" + line;
    using_history();
    HIST_ENTRY *old = replace_history_entry(history_length - 1, joined.c_str(), e->data);
    if (old) free_history_entry(old);
    return;
  }

  // Join with bash's history_delimiting_chars, approximately: no semicolon
  // after an operator or after a keyword that a command follows directly.
  std::string delim = "; ";
  std::string t = cur;
  while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
  if (!t.empty() && t.back() == '\\' && (t.size() < 2 || t[t.size() - 2] != '\\')) {
    // The previous line ended in an escaped newline: splice directly.
    t.pop_back();
    cur = t;
    delim = "";
  } else if (!t.empty() && std::strchr(";&|({", t.back())) {
    delim = " ";
  } else {
    size_t ws = t.find_last_of(" \t;");
    std::string w = ws == std::string::npos ? t : t.substr(ws + 1);
    if (w == "do" || w == "then" || w == "else" || w == "elif" || w == "in" ||
        w == "while" || w == "until" || w == "if" || w == "for" || w == "case" ||
        w == "select" || w == "function")
      delim = " ";
  }
  if (line.find_first_not_of(" \t") == std::string::npos) return;  // blank line

  std::string joined = cur + delim + line;
  using_history();
  HIST_ENTRY *old = replace_history_entry(history_length - 1, joined.c_str(),
                                          e->data);
  if (old) free_history_entry(old);
}

// Where TEXT ends: inside a quoted string, inside a command/process
// substitution or backquotes, or in neither.  Used to decide whether bash
// would history-expand a continuation line and how it joins the command's
// history entry.
enum class OpenCtx { None, Quote, Subst };
static OpenCtx open_context(const std::string &t) {
  bool squote = false, dquote = false;
  int depth = 0;
  for (size_t i = 0; i < t.size(); i++) {
    char c = t[i];
    if (c == '\\' && !squote) { if (i + 1 < t.size()) i++; continue; }
    if (squote) { if (c == '\'') squote = false; continue; }
    if (c == '\'' && !dquote) { squote = true; continue; }
    if (c == '"') { dquote = !dquote; continue; }
    if (c == '`') { depth = depth ? depth - 1 : depth + 1; continue; }
    if (c == '(' && i > 0 && (t[i - 1] == '$' || t[i - 1] == '<' || t[i - 1] == '>')) {
      depth++;
      continue;
    }
    if (c == ')' && depth > 0) depth--;
  }
  if (squote || dquote) return OpenCtx::Quote;
  return depth > 0 ? OpenCtx::Subst : OpenCtx::None;
}

// Whether a trailing backslash in T is literal rather than a line continuation.
// It is literal when T ends inside an open single-quoted string, since backslash
// is not special there -- except when that single quote is nested inside an
// old-style backtick, whose own backslash pre-pass splices `\<newline>' away
// before the inner quoting is even seen.  A single quote inside `$(...)' (or at
// top level) keeps the backslash literal; only backticks force the splice.
// True when T's final character sits inside a comment (an unquoted `#' that
// began a word, running to end of line with no newline after it): a trailing
// backslash there is comment TEXT, never a line continuation (bash --
// `echo $(\n echo hi # \\\n)' closes fine).
static bool ends_in_comment(const std::string &t) {
  bool squote = false, dquote = false;
  char prev = '\n';
  for (size_t i = 0; i < t.size(); i++) {
    char c = t[i];
    if (squote) { if (c == '\'') squote = false; prev = c; continue; }
    if (!dquote && c == '#' && (prev == ' ' || prev == '\t' || prev == '\n' ||
                                prev == ';' || prev == '(' || prev == '&' || prev == '|')) {
      while (i < t.size() && t[i] != '\n') i++;
      if (i >= t.size()) return true;  // comment runs to the end
      prev = '\n';
      continue;
    }
    if (c == '\\') { if (i + 1 < t.size()) i++; prev = 'x'; continue; }
    if (c == '\'' && !dquote) squote = true;
    else if (c == '"') dquote = !dquote;
    prev = c;
  }
  return false;
}

static bool squote_backslash_literal(const std::string &t) {
  bool squote = false, dquote = false, btick = false;
  std::vector<bool> dq_stack;  // enclosing double-quote state per `$(' level
  char prev = '\n';  // start of input behaves like just after a newline
  for (size_t i = 0; i < t.size(); i++) {
    char c = t[i];
    if (squote) { if (c == '\'') squote = false; prev = c; continue; }
    // Outside quotes, an unquoted `#' that begins a word starts a comment
    // running to end of line; a `'' (or `"') inside it is a literal comment
    // character, NOT a quote (else `# it's x' would look like an open quote and
    // suppress a genuine line continuation further down -- e.g. config.guess).
    // Only a `#' after a blank or at the start of a line begins a comment; one
    // stuck to a preceding word (`)#x', `$(cmd)#x') is part of that word.
    if (!dquote && !btick && c == '#' &&
        (prev == ' ' || prev == '\t' || prev == '\n')) {
      while (i < t.size() && t[i] != '\n') i++;
      prev = '\n';
      continue;  // the for-loop's ++i steps past the newline
    }
    if (c == '\\') { if (i + 1 < t.size()) i++; prev = 'x'; continue; }
    // A `$(' starts a fresh quoting context: single quotes are quotes again
    // even inside the enclosing double quotes (`echo "$(echo 'foo\' ...)"' --
    // quote.tests), so remember the outer state and restart.
    if (c == '$' && i + 1 < t.size() && t[i + 1] == '(' && !squote) {
      dq_stack.push_back(dquote);
      dquote = false;
      i++;
      prev = '(';
      continue;
    }
    if (c == ')' && !squote && !dq_stack.empty()) {
      dquote = dq_stack.back();
      dq_stack.pop_back();
      prev = c;
      continue;
    }
    if (c == '\'' && !dquote) squote = true;
    else if (c == '"') dquote = !dquote;
    else if (c == '`') btick = !btick;
    prev = c;
  }
  return squote && !btick;
}

int Shell::run_script_lines(const std::string &text) {
  if (is_csh()) return run_string(text);  // csh runs whole-buffer

  size_t pos = 0;
  int lineno = 0;        // 1-based physical line being read
  int pending_line = 1;  // first line of the accumulating command
  std::string pending;
  bool cont_bslash = false;  // previous line ended in a line continuation
  bool first_line_saved = false;  // this command's first line is in the history
  bool in_heredoc = false;   // the pending command has an open here-document
  bool in_heredoc_quoted = false;  // ...and its delimiter was quoted
  int st = last_status;

  auto flush = [&]() {
    cont_bslash = false;
    first_line_saved = false;
    in_heredoc = false;
    if (pending.find_first_not_of(" \t\n") == std::string::npos) {
      pending.clear();
      return;
    }
    lineno_base = pending_line - 1;
    // bash reads file input with a guaranteed trailing newline; without it a
    // here-document whose delimiter is the last line of the file would be
    // (wrongly) reported as delimited by end-of-file.
    {
      // ... except after an odd run of backslashes: the synthetic newline
      // would turn the final `\' into a line continuation and swallow it,
      // where bash keeps it literal (`sh -c 'echo escape\'' -- quote.tests).
      size_t tb = 0;
      while (tb < pending.size() && pending[pending.size() - 1 - tb] == '\\') tb++;
      if (pending.back() != '\n' && tb % 2 == 0) pending += '\n';
    }
    st = run_string(pending);
    lineno_base = 0;
    hist_cur_cmd_index = -1;
    pending.clear();
  };

  while (pos < text.size() && !exiting && !stdin_source_changed) {
    size_t nl = text.find('\n', pos);
    std::string line = (nl == std::string::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
    bool line_had_newline = nl != std::string::npos;
    pos = (nl == std::string::npos) ? text.size() : nl + 1;
    lineno++;

    bool fresh = pending.empty();
    if (fresh) pending_line = lineno;

    // With `-o history' each line is preprocessed as it is read (bash's
    // pre_process_line): `!' expansion with the expanded line echoed to
    // stderr, then recorded in the history -- all before execution.
    if (opt_history && line.find_first_not_of(" \t") != std::string::npos) {
      OpenCtx ctx = fresh ? OpenCtx::None : open_context(pending);
      if (opt_histexpand && ctx == OpenCtx::None) {
        sync_histchars();
        cur_lineno = lineno;  // the expansion error prefix names this line
        // Expanding a later line of a multi-line command: history references
        // must resolve to the previous complete command, not the partial one,
        // so lift the partial entry out for the duration of the expansion.
        HIST_ENTRY *hidden = nullptr;
        if (!fresh && first_line_saved && history_length > 0) {
          hidden = remove_history(history_length - 1);
          using_history();
        }
        char *hv = nullptr;
        int hr = history_expand(const_cast<char *>(line.c_str()), &hv);
        if (hidden) {
          add_history(hidden->line);
          free_history_entry(hidden);
          using_history();
        }
        if (hr < 0) {
          std::fprintf(stderr, "%s%s\n", err_prefix().c_str(), hv ? hv : "history expansion failed");
          std::free(hv);
          st = last_status = 1;
          continue;  // the failed line is neither run nor recorded
        }
        if (hr != 0 && hv) std::fprintf(stderr, "%s\n", hv);  // echo the expansion
        if (hv) { line = hv; std::free(hv); }
        if (hr == 2) {  // `:p' modifier: print and record, don't execute
          if (fresh) add_history_line(line);
          continue;
        }
      }
      if (fresh) {
        first_line_saved = add_history_line(line);
        hist_cur_cmd_index = first_line_saved ? history_length - 1 : -1;
      } else if (first_line_saved &&
                 (in_heredoc || ctx == OpenCtx::Quote ||
                  !(line.find_first_not_of(" \t") != std::string::npos &&
                    line[line.find_first_not_of(" \t")] == '#'))) {
        // shopt cmdhist: later lines of the command extend its history entry
        // (pure comment lines are not appended).  Here-document lines and
        // lines continuing a quoted string keep their line structure:
        // newline joins, with a here-document body's trailing newline
        // preserved when the delimiter closes it.
        append_history_line(line, in_heredoc || ctx == OpenCtx::Quote);
      }
    }

    if (cont_bslash) pending += line;
    else if (pending.empty()) pending = line;
    else pending += "\n" + line;

    // A trailing (unescaped) backslash continues onto the next line -- but not
    // inside a single-quoted string, where the backslash and newline are both
    // literal and the quote itself is what carries the command onto the next
    // line (handled below by the incomplete-parse path).
    size_t nbs = 0;
    while (nbs < pending.size() && pending[pending.size() - 1 - nbs] == '\\') nbs++;
    // A trailing backslash inside a QUOTED here-document is a literal body
    // character, not a line continuation.  In an unquoted here-document bash
    // still splices `\<newline>' (before even checking for the delimiter), so
    // only suppress the continuation for a quoted delimiter.
    // A pending here-document with a QUOTED delimiter makes a trailing `\'
    // literal body text.  in_heredoc reflects the PREVIOUS parse, so also ask
    // the parse of the text INCLUDING this line -- the `<<' may have been read
    // only now (`x=$(cat <<'"'"'EOT'"'"'' then a body line -- comsub4.sub).
    bool hd_quoted_now = in_heredoc && in_heredoc_quoted;
    if (nbs % 2 == 1 && !hd_quoted_now) {
      ParseResult hc = aliases_active()
                           ? parse_with_aliases(pending, aliases, global_aliases,
                                                suffix_aliases, opt_posix)
                           : parse(pending, opt_posix);
      hd_quoted_now = hc.heredoc_eof && hc.heredoc_eof_quoted;
    }
    // A `\' before a NEWLINE is a line continuation even at end of file
    // (parser1.sub splices `echo AAA\' + newline into `echo AAA'); a `\'
    // that ends the input with no newline at all is literal (`sh -c 'echo
    // escape\'' prints the backslash -- quote.tests).
    if (nbs % 2 == 1 && line_had_newline && !squote_backslash_literal(pending) &&
        !ends_in_comment(pending) && !hd_quoted_now) {
      pending.pop_back();
      cont_bslash = true;
      continue;
    }
    cont_bslash = false;

    if (pos < text.size()) {
      // Completeness must be judged on the alias-expanded text: an alias can
      // open a construct the raw line doesn't show (`alias al=' '; al for x
      // in v' needs the do/done lines) or carry an open quote into the
      // following text.
      ParseResult chk =
          aliases_active()
              ? parse_with_aliases(pending, aliases, global_aliases, suffix_aliases, opt_posix)
              : parse(pending, opt_posix);
      bool was_heredoc = in_heredoc;
      in_heredoc = chk.heredoc_eof;
      in_heredoc_quoted = chk.heredoc_eof_quoted;
      if (was_heredoc && !in_heredoc && first_line_saved)
        append_history_line("", true);  // the closing delimiter's newline
      if (chk.incomplete) continue;
    } else {
      in_heredoc = false;
    }
    flush();
    // A non-interactive shell stops reading input after a syntax error (bash
    // exits with status 2; the -c and script readers both abort here).
    if (had_parse_error && !interactive) return st;
  }
  if (!exiting) flush();  // whatever remains (an incomplete tail still errors)
  return st;
}

bool Shell::valid_alias_name(const std::string &name) {
  if (name.empty()) return false;
  for (char c : name)
    if (std::strchr(" \t\n;|&()<>'\"`\\$/", static_cast<unsigned char>(c))) return false;
  return true;
}

bool Shell::aliases_active() const {
  // Aliases are expanded only when interactive or `shopt -s expand_aliases';
  // posix mode enables alias expansion even in non-interactive shells.
  bool expand_al = interactive || opt_posix;
  auto eit = shopt_opts.find("expand_aliases");
  if (eit != shopt_opts.end() && eit->second) expand_al = true;
  return expand_al &&
         (!aliases.empty() || !global_aliases.empty() || !suffix_aliases.empty());
}

int Shell::run_string(const std::string &script) {
  if (is_csh()) return run_csh(*this, script);  // csh is a different language
  had_parse_error = false;
  ParseResult r = aliases_active()
                      ? parse_with_aliases(script, aliases, global_aliases, suffix_aliases,
                                           opt_posix)
                      : parse(script, opt_posix);
  if (!r.ok) {
    // bash's format: `NAME: [CONTEXT: ][-c: ]line N: syntax error...' per
    // message line; "near unexpected token" joins without a colon, and the
    // offending source line is echoed after it.
    std::string ctx;
    if (!error_context.empty()) ctx = error_context + ": ";
    else if (invocation_char == 'c') ctx = "-c: ";
    std::string pfx = shell_name + ": " + ctx + "line " +
                      std::to_string(lineno_base + (r.error_line > 0 ? r.error_line : 1)) +
                      ": ";
    // An EOF-delimited here-document inside the failed construct still warns,
    // before the syntax error (heredoc3.sub's `(cat <<EOF' at end of file).
    if (r.heredoc_eof) {
      int save_ln = cur_lineno;
      int nlines = static_cast<int>(std::count(script.begin(), script.end(), '\n'));
      cur_lineno = lineno_base + nlines +
                   ((!script.empty() && script.back() == '\n') ? 0 : 1);
      std::fprintf(stderr,
                   "%swarning: here-document at line %d delimited by end-of-file (wanted `%s')\n",
                   err_prefix().c_str(), lineno_base + r.heredoc_eof_line,
                   r.heredoc_eof_delim.c_str());
      cur_lineno = save_ln;
    }
    size_t p0 = 0;
    while (p0 <= r.error.size()) {
      size_t nl = r.error.find('\n', p0);
      std::string line = r.error.substr(p0, nl == std::string::npos ? std::string::npos : nl - p0);
      // Rebase an embedded "... command on line N" to file coordinates.
      size_t cp = line.rfind("' command on line ");
      if (cp != std::string::npos && lineno_base > 0)
        line = line.substr(0, cp + 18) +
               std::to_string(lineno_base + std::atoi(line.c_str() + cp + 18));
      // bash prints its conditional-expression diagnostics, and the
      // unterminated-EOF ones, without a `syntax error' prefix -- they are
      // complete sentences already, and a `syntax error near ...' line follows.
      auto starts = [&line](const char *pfx2) {
        return line.rfind(pfx2, 0) == 0;
      };
      auto bare = [&starts]() {
        return starts("unexpected EOF") || starts("unexpected token `") ||
               starts("unexpected argument `") ||
               starts("syntax error in conditional expression");
      };
      if (bare()) {
        std::fprintf(stderr, "%s%s\n", pfx.c_str(), line.c_str());  // no `syntax error'
      } else {
        const char *sep = line.compare(0, 5, "near ") == 0 ? " " : ": ";
        std::fprintf(stderr, "%ssyntax error%s%s\n", pfx.c_str(), sep, line.c_str());
      }
      if (nl == std::string::npos) break;
      p0 = nl + 1;
    }
    // Echo the offending source line whenever a `near ...' line was printed --
    // it may be the second line of a conditional-expression report.
    bool has_near = r.error.compare(0, 5, "near ") == 0 ||
                    r.error.find("\nnear ") != std::string::npos;
    if (has_near && r.error_line > 0) {
      // Echo the offending source line, as bash does.
      size_t start = 0;
      for (int k = 1; k < r.error_line && start != std::string::npos; k++) {
        start = script.find('\n', start);
        if (start != std::string::npos) start++;
      }
      if (start != std::string::npos) {
        size_t fin = script.find('\n', start);
        std::string src = script.substr(start, fin == std::string::npos ? std::string::npos
                                                                        : fin - start);
        std::fprintf(stderr, "%s`%s'\n", pfx.c_str(), src.c_str());
      }
    }
    // A compound-assignment syntax error (`a=(x & y)') is reported by bash with
    // status 1, not the usual 2 -- and does not stop a non-interactive reader.
    had_parse_error = !r.assign_error;
    last_status = r.assign_error ? 1 : 2;
    return last_status;
  }
  if (r.comsub_unterm) {
    int save_ln = cur_lineno;
    cur_lineno = lineno_base + r.comsub_unterm_line;
    std::fprintf(stderr, "%swarning: command substitution: %d unterminated here-document%s\n",
                 err_prefix().c_str(), r.comsub_unterm, r.comsub_unterm > 1 ? "s" : "");
    cur_lineno = save_ln;
  }
  if (r.heredoc_eof) {  // run anyway, with bash's warning
    // bash's warning prefix names the line where end-of-file was reached
    // (one past the chunk's last line), not the here-document's start.
    int save_ln = cur_lineno;
    int nlines = static_cast<int>(std::count(script.begin(), script.end(), '\n'));
    // Where end-of-file was reached: input ending mid-line counts the partial
    // line (a comsub's `EOF)' final line); a trailing newline does not start
    // a new one (a file's last line is the EOF line).
    cur_lineno = lineno_base + nlines +
                 ((!script.empty() && script.back() == '\n') ? 0 : 1);
    std::fprintf(stderr,
                 "%swarning: here-document at line %d delimited by end-of-file (wanted `%s')\n",
                 err_prefix().c_str(), lineno_base + r.heredoc_eof_line,
                 r.heredoc_eof_delim.c_str());
    cur_lineno = save_ln;
  }
  if (!r.command) { subshell_leaf = false; return last_status; }
  const Command *c = r.command.get();
  retained.push_back(std::move(r.command));
  // A disposable subshell child (command substitution) whose whole body is a
  // single simple command lets that command's external exec replace us.
  if (subshell_leaf && dynamic_cast<const SimpleCommand *>(c)) can_exec_replace = true;
  subshell_leaf = false;
  Executor ex(*this);
  int st = ex.run(c);
  can_exec_replace = false;
  last_status = st;
  run_pending_traps();  // deliver signals received during the final command
  last_status = st;
  arith_abort = false;  // the unwind ends with this command list
  // had_parse_error reports THIS string's parse only: a syntax error inside a
  // nested run (eval, source, a trap body) does not abort the caller's reader
  // -- bash keeps going after `eval "do"' but stops its own input after a
  // top-level syntax error.
  had_parse_error = false;
  return st;
}

std::string Shell::run_and_capture_inproc(const std::string &script, int *status,
                                          bool valsub) {
  // ${| cmd; } valsub captures the body's $REPLY rather than its stdout, which
  // is left connected to the shell's real stdout.
  std::fflush(stdout);
  int saved = -1;
  FILE *tf = nullptr;
  if (!valsub) {
    saved = dup(STDOUT_FILENO);
    tf = std::tmpfile();
    if (!tf) { if (status) *status = 1; return std::string(); }
    dup2(fileno(tf), STDOUT_FILENO);
  }
  int saved_base = lineno_base;  // run at the enclosing command's line
  lineno_base = cur_lineno - 1;
  // A valsub's $REPLY is private: bash saves and restores it around the body,
  // so `${| REPLY=x; }' leaves the caller's REPLY untouched (comsub26.sub).
  bool had_reply = false;
  Variable saved_reply;
  if (valsub) {
    auto rit = vars.find("REPLY");
    if (rit != vars.end()) { had_reply = true; saved_reply = rit->second; }
  }
  // A funsub runs in a fresh local scope (so a `local' inside it does not leak)
  // and is a return boundary like a function body: `return' ends the funsub
  // (its status becomes the funsub's) rather than unwinding the caller.
  bool saved_returning = returning;
  returning = false;
  // Like a command substitution, a funsub does not inherit errexit unless
  // `shopt -s inherit_errexit' or POSIX mode says otherwise: `${ false;
  // echo after; }' still runs the echo under `set -e' (comsub22.sub).
  bool saved_errexit = opt_errexit;
  if (opt_errexit && !opt_posix && !shopt_opts["inherit_errexit"]) opt_errexit = false;
  push_scope();
  int st = run_string(script);
  pop_scope();
  opt_errexit = saved_errexit;
  if (returning) { st = exit_status; returning = false; }
  returning = saved_returning;
  lineno_base = saved_base;
  if (valsub) {
    if (status) *status = st;
    std::string reply = get("REPLY");
    if (had_reply) vars["REPLY"] = saved_reply;
    else vars.erase("REPLY");
    return reply;
  }
  std::fflush(stdout);
  dup2(saved, STDOUT_FILENO);
  close(saved);
  if (status) *status = st;
  std::rewind(tf);
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, tf)) > 0) out.append(buf, n);
  std::fclose(tf);
  while (!out.empty() && out.back() == '\n') out.pop_back();
  return out;
}

std::string Shell::run_and_capture(const std::string &script, int *status) {
  // bash optimization: when the entire command-substitution body is a lone input
  // redirection (`$(< file)'), the substitution expands to the contents of the
  // file -- no `cat' is forked.  It applies only to this exact shape: a single
  // `<' with just a filename after it and nothing else (`$(echo x; < f)' and
  // `$(< f cmd)' are ordinary commands).
  {
    size_t a = script.find_first_not_of(" \t\n");
    if (a != std::string::npos && script[a] == '<' &&
        (a + 1 >= script.size() ||
         (script[a + 1] != '<' && script[a + 1] != '(' && script[a + 1] != '>'))) {
      size_t p = a + 1;
      while (p < script.size() && (script[p] == ' ' || script[p] == '\t')) p++;
      size_t start = p;
      bool simple = true;
      for (; p < script.size(); p++) {
        char c = script[p];
        if (c == '\'') {
          size_t e = script.find('\'', p + 1);
          if (e == std::string::npos) { simple = false; break; }
          p = e;
        } else if (c == '"') {
          size_t e = script.find('"', p + 1);
          if (e == std::string::npos) { simple = false; break; }
          p = e;
        } else if (c == '\\') {
          p++;
        } else if (c == ' ' || c == '\t' || c == '\n') {
          break;
        } else if (std::strchr("|&;<>()`", c)) {
          simple = false;  // another command/redirection follows: not the `< f' form
          break;
        }
      }
      std::string raw = script.substr(start, p - start);
      // Everything after the filename word must be blank for the optimization.
      bool only = script.find_first_not_of(" \t\n", p) == std::string::npos;
      if (simple && only && !raw.empty()) {
        Expander ex(*this);
        std::string path = ex.expand_no_split(raw);
        FILE *f = std::fopen(path.c_str(), "r");
        if (!f) {
          std::fprintf(stderr, "%s%s: %s\n", err_prefix().c_str(), path.c_str(),
                       std::strerror(errno));
          if (status) *status = 1;
          return std::string();
        }
        std::string out;
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
        std::fclose(f);
        if (status) *status = 0;
        while (!out.empty() && out.back() == '\n') out.pop_back();
        return out;
      }
    }
  }
  int fds[2];
  if (pipe(fds) != 0) {
    if (status) *status = 1;
    return std::string();
  }
  pid_t pid = fork();
  if (pid == 0) {
    // Child: stdout -> pipe, run the script, exit with its status.
    close(fds[0]);
    dup2(fds[1], STDOUT_FILENO);
    close(fds[1]);
    job_control = false;  // command substitution: no nested tty control
    no_current_job = true;  // bash resets the current job in the subshell
    subshell_level++;  // $BASH_SUBSHELL
    traps.erase("CHLD");  // the parent fires CHLD for the substitution as a whole
    if (!opt_functrace) traps.erase("ERR");  // ERR is not inherited without -E
    pending_sigchld = 0;
    subshell_leaf = true;  // a lone external here can exec in place (no 2nd fork)
    // Command substitution unsets errexit in the subshell unless the caller has
    // enabled `shopt -s inherit_errexit'; so `$(false; echo ok)' still runs the
    // `echo' under `set -e'.  POSIX mode inherits errexit into the subshell, so
    // there `z=$(false; echo foo)' exits silently before the `echo'.
    if (opt_errexit && !opt_posix && !shopt_opts["inherit_errexit"]) opt_errexit = false;
    // The substitution's commands run at the enclosing command's line: bash does
    // not reset the line counter for a command substitution, so $LINENO (and a
    // DEBUG trap firing inside it) reports the line where the `$(...)' appears.
    lineno_base = cur_lineno - 1;
    int st = run_string(script);
    std::fflush(stdout);
    _exit(st & 0xff);
  }
  close(fds[1]);
  std::string out;
  char buf[4096];
  ssize_t n;
  while ((n = read(fds[0], buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
  close(fds[0]);
  int wst = 0;
  waitpid(pid, &wst, 0);
  note_child_reaped();  // a command-substitution child terminated
  if (status) *status = WIFEXITED(wst) ? WEXITSTATUS(wst) : 128;
  // Strip trailing newlines, as command substitution does.
  while (!out.empty() && out.back() == '\n') out.pop_back();
  return out;
}

}  // namespace gnash::core
