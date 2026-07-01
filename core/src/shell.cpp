// shell.cpp -- interpreter state and top-level run/capture.

#include "gnash/core/shell.hpp"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>

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
}

bool Shell::is_set(const std::string &n) const { return vars.count(n) != 0; }

std::string Shell::get(const std::string &n) const {
  auto it = vars.find(n);
  if (it == vars.end()) return std::string();
  const Variable &v = it->second;
  if (v.kind == VarKind::Indexed) return v.idx.count(0) ? v.idx.at(0) : std::string();
  if (v.kind == VarKind::Assoc) return v.assoc.count("0") ? v.assoc.at("0") : std::string();
  return v.value;
}

// ---- arrays ---------------------------------------------------------------

std::vector<std::string> Shell::array_values(const std::string &n) const {
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

std::vector<std::string> Shell::array_keys(const std::string &n) const {
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

std::string Shell::array_get(const std::string &n, const std::string &sub) const {
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

void Shell::array_set(const std::string &n, const std::string &sub, const std::string &val) {
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

int Shell::array_count(const std::string &n) const {
  auto it = vars.find(n);
  if (it == vars.end()) return 0;
  const Variable &v = it->second;
  if (v.kind == VarKind::Indexed) return static_cast<int>(v.idx.size());
  if (v.kind == VarKind::Assoc) return static_cast<int>(v.assoc.size());
  return 1;
}

void Shell::make_array(const std::string &n, bool assoc) {
  Variable &v = vars[n];
  if (v.kind == VarKind::Scalar) v.kind = assoc ? VarKind::Assoc : VarKind::Indexed;
}

void Shell::array_assign(
    const std::string &n,
    const std::vector<std::pair<std::optional<std::string>, std::string>> &elems,
    bool append, bool assoc) {
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

bool Shell::get_if_set(const std::string &n, std::string &out) const {
  auto it = vars.find(n);
  if (it == vars.end()) return false;
  out = it->second.value;
  return true;
}

void Shell::set(const std::string &n, const std::string &v) {
  Variable &var = vars[n];
  if (var.readonly) return;
  var.value = v;
}

void Shell::set_exported(const std::string &n, const std::string &v) {
  Variable &var = vars[n];
  if (var.readonly) return;
  var.value = v;
  var.exported = true;
}

void Shell::export_name(const std::string &n) { vars[n].exported = true; }

void Shell::unset(const std::string &n) {
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
  ParseResult r = parse(script);
  if (!r.ok) {
    std::fprintf(stderr, "gnash: syntax error: %s\n", r.error.c_str());
    last_status = 2;
    return 2;
  }
  if (!r.command) return last_status;
  const Command *c = r.command.get();
  retained.push_back(std::move(r.command));
  Executor ex(*this);
  int st = ex.run(c);
  last_status = st;
  return st;
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
