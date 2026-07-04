// shell.cpp -- interpreter state and top-level run/capture.

#include "gnash/core/shell.hpp"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <ctime>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#include "gnash/core/csh.hpp"
#include "gnash/core/executor.hpp"
#include "gnash/core/parser.hpp"

extern "C" char **environ;

namespace gnash::core {

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
  set("PPID", std::to_string(static_cast<long>(getppid())));
  set("$", std::to_string(static_cast<long>(getpid())));
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
}

// Advance bash's RANDOM generator (Park-Miller minimal-standard PRNG) and
// return a value in 0..32767, matching bash 5.3 exactly for a given seed.
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
      "EPOCHSECONDS", "EPOCHREALTIME"};
  return names;
}

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
  return false;
}

namespace {
volatile sig_atomic_t g_trap_pending[NSIG];

void trap_signal_handler(int sig) {
  if (sig > 0 && sig < NSIG) g_trap_pending[sig] = 1;
}

// Canonical trap name for a signal number (matching how `trap' stores keys).
const char *signum_to_trapname(int sig) {
  switch (sig) {
    case SIGHUP: return "HUP";   case SIGINT: return "INT";
    case SIGQUIT: return "QUIT"; case SIGTERM: return "TERM";
    case SIGUSR1: return "USR1"; case SIGUSR2: return "USR2";
    case SIGALRM: return "ALRM"; case SIGPIPE: return "PIPE";
    case SIGTSTP: return "TSTP"; case SIGCONT: return "CONT";
    case SIGCHLD: return "CHLD";
    default: return nullptr;
  }
}
}  // namespace

void Shell::set_signal_trap(int signo, bool active) {
  if (signo <= 0 || signo >= NSIG) return;
  struct sigaction sa;
  std::memset(&sa, 0, sizeof sa);
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;  // let blocking waits resume; traps run between commands
  sa.sa_handler = active ? trap_signal_handler : SIG_DFL;
  sigaction(signo, &sa, nullptr);
}

void Shell::run_debug_trap(const std::string &cmd_text) {
  auto it = traps.find("DEBUG");
  if (it == traps.end() || in_debug_trap) return;
  // Without functrace, the DEBUG trap fires only outside functions.
  if (!opt_functrace && in_function()) return;
  in_debug_trap = true;
  bash_command = cmd_text;
  int saved = last_status;  // $? inside the trap is the previous command's status
  int saved_line = cur_lineno;  // the trap must not leak its own line numbers
  std::string body = it->second;
  run_string(body);
  last_status = saved;  // the trap does not alter $? for the upcoming command
  cur_lineno = saved_line;
  in_debug_trap = false;
}

void Shell::run_pending_traps() {
  if (in_trap) return;
  for (int s = 1; s < NSIG; s++) {
    if (!g_trap_pending[s]) continue;
    g_trap_pending[s] = 0;
    const char *nm = signum_to_trapname(s);
    if (!nm) continue;
    auto it = traps.find(nm);
    if (it == traps.end() || it->second.empty()) continue;
    in_trap = true;
    int saved = last_status;  // the interrupted command's $?
    std::string cmd = it->second;
    run_string(cmd);
    last_status = saved;
    in_trap = false;
  }
}

void Shell::reap_procsubs(size_t from) {
  for (size_t k = from; k < procsubs.size(); k++) {
    if (procsubs[k].fd >= 0) close(procsubs[k].fd);
    int st = 0;
    waitpid(static_cast<pid_t>(procsubs[k].pid), &st, 0);
  }
  if (from < procsubs.size()) procsubs.resize(from);
}

std::string Shell::deref(const std::string &n) const {
  std::string cur = n;
  for (int guard = 0; guard < 100; guard++) {
    auto it = vars.find(cur);
    if (it == vars.end() || !it->second.nameref) return cur;
    const std::string &tgt = it->second.value;
    if (tgt.empty() || tgt == cur) return cur;  // self/empty ref: stop
    cur = tgt;
  }
  return cur;
}

bool Shell::is_set(const std::string &n_in) const {
  std::string n = deref(n_in);
  return vars.count(n) != 0;
}

std::string Shell::get(const std::string &n_in) const {
  std::string n = deref(n_in);
  auto it = vars.find(n);
  if (it == vars.end()) return std::string();
  const Variable &v = it->second;
  if (v.kind == VarKind::Indexed) return v.idx.count(0) ? v.idx.at(0) : std::string();
  if (v.kind == VarKind::Assoc) return v.assoc.count("0") ? v.assoc.at("0") : std::string();
  return v.value;
}

// ---- arrays ---------------------------------------------------------------

std::vector<std::string> Shell::array_values(const std::string &n_in) const {
  std::string n = deref(n_in);
  std::vector<std::string> out;
  auto it = vars.find(n);
  if (it == vars.end()) return out;
  const Variable &v = it->second;
  if (v.kind == VarKind::Indexed)
    for (const auto &kv : v.idx) out.push_back(kv.second);
  else if (v.kind == VarKind::Assoc)
    for (const auto &kv : v.assoc) out.push_back(kv.second);
  else
    out.push_back(v.value);
  return out;
}

std::vector<std::string> Shell::array_keys(const std::string &n_in) const {
  std::string n = deref(n_in);
  std::vector<std::string> out;
  auto it = vars.find(n);
  if (it == vars.end()) return out;
  const Variable &v = it->second;
  if (v.kind == VarKind::Indexed)
    for (const auto &kv : v.idx) out.push_back(std::to_string(kv.first));
  else if (v.kind == VarKind::Assoc)
    for (const auto &kv : v.assoc) out.push_back(kv.first);
  else if (!v.value.empty() || vars.count(n))
    out.push_back("0");
  return out;
}

std::string Shell::array_get(const std::string &n_in, const std::string &sub) const {
  std::string n = deref(n_in);
  auto it = vars.find(n);
  if (it == vars.end()) return std::string();
  const Variable &v = it->second;
  if (v.kind == VarKind::Assoc) return v.assoc.count(sub) ? v.assoc.at(sub) : std::string();
  bool ok = true;
  long long k = eval_arith(const_cast<Shell &>(*this), sub, &ok);
  if (!ok) k = 0;
  if (v.kind == VarKind::Indexed) return v.idx.count(k) ? v.idx.at(k) : std::string();
  return (k == 0) ? v.value : std::string();
}

void Shell::array_set(const std::string &n_in, const std::string &sub, const std::string &val) {
  std::string n = deref(n_in);
  Variable &v = vars[n];
  if (v.readonly) return;
  if (v.kind == VarKind::Assoc) {
    v.assoc[sub] = val;
    return;
  }
  v.kind = VarKind::Indexed;
  bool ok = true;
  long long k = eval_arith(*this, sub, &ok);
  if (!ok) k = 0;
  v.idx[k] = val;
  if (k == 0) v.value = val;
}

int Shell::array_count(const std::string &n_in) const {
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
  Variable &v = vars[n];
  if (v.kind == VarKind::Scalar) v.kind = assoc ? VarKind::Assoc : VarKind::Indexed;
}

void Shell::array_assign(
    const std::string &n_in,
    const std::vector<std::pair<std::optional<std::string>, std::string>> &elems,
    bool append, bool assoc) {
  std::string n = deref(n_in);
  Variable &v = vars[n];
  if (v.readonly) return;
  if (!append) {
    v.idx.clear();
    v.assoc.clear();
    v.value.clear();
  }
  v.kind = (assoc || v.kind == VarKind::Assoc) ? VarKind::Assoc : VarKind::Indexed;
  long long next = 0;
  if (v.kind == VarKind::Indexed && !v.idx.empty()) next = v.idx.rbegin()->first + 1;
  for (const auto &e : elems) {
    if (v.kind == VarKind::Assoc) {
      if (e.first) v.assoc[*e.first] = e.second;
    } else if (e.first) {
      bool ok = true;
      long long k = eval_arith(*this, *e.first, &ok);
      if (!ok) k = 0;
      v.idx[k] = e.second;
      next = k + 1;
    } else {
      v.idx[next++] = e.second;
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

  if (!src_frames.back().is_func) {  // FUNCNAME/BASH_LINENO exist only in functions
    unset("FUNCNAME"); unset("BASH_LINENO");
    return;
  }
  std::vector<std::string> names, lines;
  for (size_t i = src_frames.size(); i-- > 1;) {  // top down to frame 1 (above base)
    names.push_back(src_frames[i].name);
    lines.push_back(std::to_string(src_frames[i].line));
  }
  names.push_back("main");
  lines.push_back("0");
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

void Shell::push_scope() { local_stack.emplace_back(); }

void Shell::pop_scope() {
  if (local_stack.empty()) return;
  auto &scope = local_stack.back();
  for (auto it = scope.rbegin(); it != scope.rend(); ++it) {
    if (it->second)
      vars[it->first] = *it->second;
    else
      vars.erase(it->first);
  }
  local_stack.pop_back();
}

void Shell::make_local(const std::string &n) {
  if (local_stack.empty()) return;  // `local' outside a function: no-op scope
  auto &scope = local_stack.back();
  for (auto &e : scope)
    if (e.first == n) return;  // already made local in this scope
  auto it = vars.find(n);
  scope.emplace_back(n, it == vars.end() ? std::nullopt : std::optional<Variable>(it->second));
  vars[n] = Variable{};  // fresh empty local
}

bool Shell::get_if_set(const std::string &n_in, std::string &out) const {
  std::string n = deref(n_in);
  auto it = vars.find(n);
  if (it == vars.end()) return false;
  out = it->second.value;
  return true;
}

void Shell::set(const std::string &n_in, const std::string &v) {
  std::string n = deref(n_in);
  // Assigning BASH_ARGV0 resets $0 (and the name used in error messages), as
  // bash does; still stored so `$BASH_ARGV0' reads back the value.
  if (n == "BASH_ARGV0") { arg0 = v; shell_name = v; }
  // Assigning to a dynamic variable seeds/rebases it rather than storing.
  if (n == "RANDOM") {
    rand_seed = static_cast<unsigned long>(std::strtoul(v.c_str(), nullptr, 10));
    rand_seeded = true;
    return;
  }
  if (n == "SECONDS") {
    seconds_base = static_cast<long long>(std::time(nullptr)) -
                   std::strtoll(v.c_str(), nullptr, 10);
    return;
  }
  Variable &var = vars[n];
  if (var.readonly) {
    std::fprintf(stderr, "%s%s: readonly variable\n", err_prefix().c_str(), n.c_str());
    return;
  }
  var.value = v;
}

void Shell::set_exported(const std::string &n_in, const std::string &v) {
  std::string n = deref(n_in);
  Variable &var = vars[n];
  if (var.readonly) return;
  var.value = v;
  var.exported = true;
}

void Shell::export_name(const std::string &n) { vars[n].exported = true; }

void Shell::unset(const std::string &n_in) {
  // `unset name' on a nameref removes the target; only `unset -n' (not modeled
  // here) removes the nameref itself.  Following the ref matches common usage.
  std::string n = deref(n_in);
  auto it = vars.find(n);
  if (it != vars.end() && !it->second.readonly) vars.erase(it);
}

std::string Shell::ifs() const {
  auto it = vars.find("IFS");
  return it == vars.end() ? std::string(" \t\n") : it->second.value;
}

std::vector<std::string> Shell::environ_block() const {
  std::vector<std::string> out;
  for (const auto &kv : vars)
    if (kv.second.exported) out.push_back(kv.first + "=" + kv.second.value);
  return out;
}

int Shell::run_string(const std::string &script) {
  if (is_csh()) return run_csh(*this, script);  // csh is a different language
  // Aliases are expanded only when interactive or `shopt -s expand_aliases'.
  bool expand_al = interactive;
  auto eit = shopt_opts.find("expand_aliases");
  if (eit != shopt_opts.end() && eit->second) expand_al = true;
  ParseResult r = (expand_al && !aliases.empty()) ? parse_with_aliases(script, aliases)
                                                  : parse(script);
  if (!r.ok) {
    std::fprintf(stderr, "gnash: syntax error: %s\n", r.error.c_str());
    last_status = 2;
    return 2;
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
  return st;
}

std::string Shell::run_and_capture_inproc(const std::string &script, int *status) {
  std::fflush(stdout);
  int saved = dup(STDOUT_FILENO);
  FILE *tf = std::tmpfile();
  if (!tf) { if (status) *status = 1; return std::string(); }
  int tfd = fileno(tf);
  dup2(tfd, STDOUT_FILENO);
  int st = run_string(script);
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
    subshell_level++;  // $BASH_SUBSHELL
    subshell_leaf = true;  // a lone external here can exec in place (no 2nd fork)
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
  if (status) *status = WIFEXITED(wst) ? WEXITSTATUS(wst) : 128;
  // Strip trailing newlines, as command substitution does.
  while (!out.empty() && out.back() == '\n') out.pop_back();
  return out;
}

}  // namespace gnash::core
