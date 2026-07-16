// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// shell.cpp -- interpreter state and top-level run/capture.

#include "gnash/core/shell.hpp"

#include <algorithm>
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
  set("OPTIND", "1");  // bash initializes getopts state at startup
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
      "EPOCHSECONDS", "EPOCHREALTIME", "BASH_MONOSECONDS", "HISTCMD",
      "BASHOPTS", "BASH_ALIASES", "BASH_CMDS", "BASH_ARGC", "BASH_ARGV"};
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
    for (const auto &kv : aliases) pairs.emplace_back(kv.first, kv.second);
    return true;
  }
  if (name == "BASH_CMDS") {
    for (const auto &kv : hashed) pairs.emplace_back(kv.first, kv.second);
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

int Shell::run_debug_trap(const std::string &cmd_text) {
  auto it = traps.find("DEBUG");
  if (it == traps.end() || in_debug_trap) return 0;
  // Without functrace, the DEBUG trap fires only outside functions.
  if (!opt_functrace && in_function()) return 0;
  in_debug_trap = true;
  bash_command = cmd_text;
  int saved = last_status;  // $? inside the trap is the previous command's status
  int saved_line = cur_lineno;  // the trap must not leak its own line numbers
  std::string body = it->second;
  int st = run_string(body);
  last_status = saved;  // the trap does not alter $? for the upcoming command
  cur_lineno = saved_line;
  in_debug_trap = false;
  return st;
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
  // BASH_ALIASES/BASH_CMDS always exist; BASH_ARGC/BASH_ARGV exist while their
  // (extdebug-only) view is non-empty.
  if (n_in == "BASH_ALIASES" || n_in == "BASH_CMDS") return true;
  if (n_in == "BASH_ARGC" || n_in == "BASH_ARGV") return !argframes.empty();
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
std::vector<std::string> Shell::assoc_order(const Variable &v) {
  const unsigned kBuckets = 1024;
  std::vector<std::string> keys;
  keys.reserve(v.assoc.size());
  // Insertion order: keys recorded in assoc_seq, then any stragglers not yet
  // tracked (defensive, so a missed seq update never drops a key).
  std::vector<const std::string *> ins;
  for (const auto &k : v.assoc_seq)
    if (v.assoc.count(k)) ins.push_back(&k);
  for (const auto &kv : v.assoc) {
    bool seen = false;
    for (const auto *p : ins) if (*p == kv.first) { seen = true; break; }
    if (!seen) ins.push_back(&kv.first);
  }
  struct E { unsigned bucket; size_t seq; const std::string *key; };
  std::vector<E> es;
  es.reserve(ins.size());
  for (size_t s = 0; s < ins.size(); s++)
    es.push_back({assoc_hash_string(*ins[s]) & (kBuckets - 1), s, ins[s]});
  std::stable_sort(es.begin(), es.end(), [](const E &a, const E &b) {
    if (a.bucket != b.bucket) return a.bucket < b.bucket;
    return a.seq > b.seq;  // newest-first within a bucket
  });
  for (const auto &e : es) keys.push_back(*e.key);
  return keys;
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
  if (v.kind == VarKind::Indexed) return v.idx.count(k) ? v.idx.at(k) : std::string();
  return (k == 0) ? v.value : std::string();
}

void Shell::array_set(const std::string &n_in, const std::string &sub, const std::string &val) {
  std::string n = deref(n_in);
  // BASH_CMDS is the live command hash: BASH_CMDS[name]=value adds a hash
  // entry.  A `/' value is rejected in a restricted shell; a value without a
  // `/' is resolved through $PATH (and must be found) before it is stored.
  if (n == "BASH_CMDS") {
    if (val.find('/') != std::string::npos) {
      if (opt_restricted) {
        std::fprintf(stderr, "%s%s: restricted\n", err_prefix().c_str(), val.c_str());
        return;
      }
      hashed[sub] = val;
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
    hashed[sub] = full;
    return;
  }
  Variable &v = vars[n];
  if (v.readonly) return;
  if (v.kind == VarKind::Assoc) {
    assoc_put(v, sub, val);
    return;
  }
  v.kind = VarKind::Indexed;
  bool ok = true;
  long long k = eval_arith(*this, sub, &ok);
  if (!ok) k = 0;
  v.idx[k] = val;
  if (k == 0) v.value = val;
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
    v.assoc_seq.clear();
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
      if (elems[x].first) { assoc_put(v, *elems[x].first, elems[x].second); continue; }
      const std::string &key = elems[x].second;
      assoc_put(v, key, (x + 1 < elems.size()) ? elems[x + 1].second : std::string());
      x++;  // consumed the paired value
    }
    return;
  }
  for (const auto &e : elems) {
    if (v.kind == VarKind::Assoc) {
      if (e.first) assoc_put(v, *e.first, e.second);
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

void Shell::push_scope() {
  local_stack.emplace_back();
  getopt_scope_saves.emplace_back();  // no OPTIND localized here yet
}

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

void Shell::make_local(const std::string &n) {
  if (local_stack.empty()) return;  // `local' outside a function: no-op scope
  auto &scope = local_stack.back();
  for (auto &e : scope)
    if (e.first == n) return;  // already made local in this scope
  auto it = vars.find(n);
  scope.emplace_back(n, it == vars.end() ? std::nullopt : std::optional<Variable>(it->second));
  vars[n] = Variable{};  // fresh empty local
  // Localizing OPTIND saves and resets the getopts scan state (bash restores
  // it when the function returns).
  if (n == "OPTIND" && !getopt_scope_saves.empty() && !getopt_scope_saves.back()) {
    getopt_scope_saves.back() = std::make_tuple(getopt_charidx, getopt_curarg, getopt_optind);
    getopt_charidx = 1;
    getopt_curarg.clear();
    getopt_optind = 0;
  }
}

bool Shell::get_if_set(const std::string &n_in, std::string &out) const {
  std::string n = deref(n_in);
  auto it = vars.find(n);
  if (it == vars.end()) return false;
  const Variable &v = it->second;
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

bool Shell::set(const std::string &n_in, const std::string &v) {
  std::string n = deref(n_in);
  // Assigning BASH_ARGV0 resets $0 (and the name used in error messages), as
  // bash does; still stored so `$BASH_ARGV0' reads back the value.
  if (n == "BASH_ARGV0") { arg0 = v; shell_name = v; }
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
  Variable &var = vars[n];
  if (var.readonly) {
    std::fprintf(stderr, "%s%s: readonly variable\n", err_prefix().c_str(), n.c_str());
    return false;
  }
  var.value = v;
  // `declare -u' / `-l' fold the value's case on every assignment.
  if (var.ucase)
    for (char &c : var.value) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  else if (var.lcase)
    for (char &c : var.value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
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
}

void Shell::export_name(const std::string &n) { vars[n].exported = true; }

void Shell::unset(const std::string &n_in, bool force) {
  if (n_in == "HISTSIZE" && history_loaded) unstifle_history();
  // `unset name' on a nameref removes the target; only `unset -n' (not modeled
  // here) removes the nameref itself.  Following the ref matches common usage.
  // FORCE mirrors bash's unbind_variable_noref: remove the named variable
  // itself (no nameref following) even when it is readonly.
  std::string n = force ? n_in : deref(n_in);
  auto it = vars.find(n);
  if (it != vars.end() && (force || !it->second.readonly)) vars.erase(it);
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

void Shell::set_personality(const std::string &name) {
  personality_name = name;
  if (name == "zsh") persona = Persona::Zsh;
  else if (name == "ash" || name == "dash" || name == "sh") persona = Persona::Ash;
  else if (name == "ksh" || name == "ksh93" || name == "mksh" || name == "pdksh" ||
           name == "rksh")
    persona = Persona::Ksh;
  else if (name == "csh" || name == "tcsh") persona = Persona::Csh;
  else persona = Persona::Bash;
  set("GNASH_PERSONALITY", name);

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
    // ash is minimal: it advertises no BASH_/ZSH_ identity variables.
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

int Shell::run_script_lines(const std::string &text) {
  if (is_csh()) return run_string(text);  // csh runs whole-buffer

  size_t pos = 0;
  int lineno = 0;        // 1-based physical line being read
  int pending_line = 1;  // first line of the accumulating command
  std::string pending;
  bool cont_bslash = false;  // previous line ended in a line continuation
  bool first_line_saved = false;  // this command's first line is in the history
  bool in_heredoc = false;   // the pending command has an open here-document
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
    st = run_string(pending);
    lineno_base = 0;
    hist_cur_cmd_index = -1;
    pending.clear();
  };

  while (pos < text.size() && !exiting) {
    size_t nl = text.find('\n', pos);
    std::string line = (nl == std::string::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
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

    // A trailing (unescaped) backslash continues onto the next line.
    size_t nbs = 0;
    while (nbs < pending.size() && pending[pending.size() - 1 - nbs] == '\\') nbs++;
    if (nbs % 2 == 1) {
      pending.pop_back();
      cont_bslash = true;
      continue;
    }
    cont_bslash = false;

    if (pos < text.size()) {
      ParseResult chk = parse(pending);
      bool was_heredoc = in_heredoc;
      in_heredoc = chk.heredoc_eof;
      if (was_heredoc && !in_heredoc && first_line_saved)
        append_history_line("", true);  // the closing delimiter's newline
      if (chk.incomplete) continue;
    } else {
      in_heredoc = false;
    }
    flush();
  }
  if (!exiting) flush();  // whatever remains (an incomplete tail still errors)
  return st;
}

int Shell::run_string(const std::string &script) {
  if (is_csh()) return run_csh(*this, script);  // csh is a different language
  // Aliases are expanded only when interactive or `shopt -s expand_aliases'.
  bool expand_al = interactive;
  auto eit = shopt_opts.find("expand_aliases");
  if (eit != shopt_opts.end() && eit->second) expand_al = true;
  bool have_aliases = !aliases.empty() || !global_aliases.empty() || !suffix_aliases.empty();
  ParseResult r = (expand_al && have_aliases)
                      ? parse_with_aliases(script, aliases, global_aliases, suffix_aliases)
                      : parse(script);
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
    size_t p0 = 0;
    while (p0 <= r.error.size()) {
      size_t nl = r.error.find('\n', p0);
      std::string line = r.error.substr(p0, nl == std::string::npos ? std::string::npos : nl - p0);
      if (line.compare(0, 14, "unexpected EOF") == 0) {
        std::fprintf(stderr, "%s%s\n", pfx.c_str(), line.c_str());  // no `syntax error'
      } else {
        const char *sep = line.compare(0, 5, "near ") == 0 ? " " : ": ";
        std::fprintf(stderr, "%ssyntax error%s%s\n", pfx.c_str(), sep, line.c_str());
      }
      if (nl == std::string::npos) break;
      p0 = nl + 1;
    }
    if (r.error.compare(0, 5, "near ") == 0 && r.error_line > 0) {
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
    last_status = 2;
    return 2;
  }
  if (r.heredoc_eof)  // run anyway, with bash's warning
    std::fprintf(stderr,
                 "%swarning: here-document at line %d delimited by end-of-file (wanted `%s')\n",
                 err_prefix().c_str(), lineno_base + r.heredoc_eof_line,
                 r.heredoc_eof_delim.c_str());
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
