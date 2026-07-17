// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// executor.cpp -- execute the command AST.

#include "gnash/core/executor.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <termios.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/wait.h>

#include "gnash/core/builtins.hpp"
#include "gnash/core/expand.hpp"
#include "gnash/core/lexer.hpp"
#include "strmatch.h"

extern "C" char **environ;

namespace gnash::core {

namespace {

struct SavedFd {
  int fd;
  int saved;  // dup of the original, or -1 if the fd was originally closed
};

// Write BODY to a temp file and return an fd open for reading at offset 0.
int heredoc_fd(const std::string &body) {
  char tmpl[] = "/tmp/gnash_hd_XXXXXX";
  int fd = mkstemp(tmpl);
  if (fd < 0) return -1;
  unlink(tmpl);
  ssize_t off = 0;
  while (off < static_cast<ssize_t>(body.size())) {
    ssize_t w = write(fd, body.data() + off, body.size() - static_cast<size_t>(off));
    if (w <= 0) break;
    off += w;
  }
  lseek(fd, 0, SEEK_SET);
  return fd;
}

void save_fd(int fd, std::vector<SavedFd> &saved) {
  int s = fcntl(fd, F_DUPFD_CLOEXEC, 10);
  saved.push_back({fd, s});
}

// Apply one redirect in-process; returns false on error.
bool apply_redirect(Shell &sh, const Redirect &r, std::vector<SavedFd> &saved) {
  Expander ex(sh);
  int target_fd = r.source_fd;
  auto redir_to = [&](int newfd, int fd) {
    save_fd(fd, saved);
    dup2(newfd, fd);
  };

  // A restricted shell forbids output redirections (creating/truncating/
  // appending files, and fd duplication that writes).
  if (sh.opt_restricted) {
    switch (r.op) {
      case RedirOp::OutputRedir:
      case RedirOp::Clobber:
      case RedirOp::AppendOutput:
      case RedirOp::InputOutput:
      case RedirOp::AndOutput:
      case RedirOp::AndAppend: {
        Expander rex(sh);
        std::string fn = rex.expand_no_split(r.target.text, true);
        std::fprintf(stderr, "%s%s: restricted: cannot redirect output\n",
                     sh.err_prefix().c_str(), fn.c_str());
        return false;
      }
      default: break;
    }
  }

  switch (r.op) {
    case RedirOp::InputRedir: {
      std::string fn = ex.expand_no_split(r.target.text, true);
      int f = open(fn.c_str(), O_RDONLY);
      if (f < 0) { std::fprintf(stderr, "%s%s: %s\n", sh.err_prefix().c_str(), fn.c_str(), std::strerror(errno)); return false; }
      redir_to(f, target_fd < 0 ? 0 : target_fd);
      close(f);
      return true;
    }
    case RedirOp::OutputRedir:
    case RedirOp::Clobber: {
      std::string fn = ex.expand_no_split(r.target.text, true);
      int f = open(fn.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
      if (f < 0) { std::fprintf(stderr, "%s%s: %s\n", sh.err_prefix().c_str(), fn.c_str(), std::strerror(errno)); return false; }
      redir_to(f, target_fd < 0 ? 1 : target_fd);
      close(f);
      return true;
    }
    case RedirOp::AppendOutput: {
      std::string fn = ex.expand_no_split(r.target.text, true);
      int f = open(fn.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0666);
      if (f < 0) { std::fprintf(stderr, "%s%s: %s\n", sh.err_prefix().c_str(), fn.c_str(), std::strerror(errno)); return false; }
      redir_to(f, target_fd < 0 ? 1 : target_fd);
      close(f);
      return true;
    }
    case RedirOp::InputOutput: {
      std::string fn = ex.expand_no_split(r.target.text, true);
      int f = open(fn.c_str(), O_RDWR | O_CREAT, 0666);
      if (f < 0) return false;
      redir_to(f, target_fd < 0 ? 0 : target_fd);
      close(f);
      return true;
    }
    case RedirOp::AndOutput:
    case RedirOp::AndAppend: {
      std::string fn = ex.expand_no_split(r.target.text, true);
      int flags = O_WRONLY | O_CREAT | (r.op == RedirOp::AndAppend ? O_APPEND : O_TRUNC);
      int f = open(fn.c_str(), flags, 0666);
      if (f < 0) return false;
      redir_to(f, 1);
      save_fd(2, saved);
      dup2(f, 2);
      close(f);
      return true;
    }
    case RedirOp::DupOutput:
    case RedirOp::DupInput: {
      std::string w = ex.expand_no_split(r.target.text);
      int deffd = (r.op == RedirOp::DupInput) ? 0 : 1;
      int fd = target_fd < 0 ? deffd : target_fd;
      if (w == "-") { save_fd(fd, saved); close(fd); return true; }
      int src = std::atoi(w.c_str());
      save_fd(fd, saved);
      dup2(src, fd);
      return true;
    }
    case RedirOp::HereDoc:
    case RedirOp::HereDocStrip: {
      std::string body = r.heredoc_quoted ? r.heredoc_body : ex.expand_heredoc(r.heredoc_body);
      int f = heredoc_fd(body);
      if (f < 0) return false;
      redir_to(f, target_fd < 0 ? 0 : target_fd);
      close(f);
      return true;
    }
    case RedirOp::HereString: {
      std::string body = ex.expand_no_split(r.target.text) + "\n";
      int f = heredoc_fd(body);
      if (f < 0) return false;
      redir_to(f, target_fd < 0 ? 0 : target_fd);
      close(f);
      return true;
    }
  }
  return true;
}

bool apply_redirects(Shell &sh, const std::vector<Redirect> &redirs, std::vector<SavedFd> &saved) {
  for (const Redirect &r : redirs)
    if (!apply_redirect(sh, r, saved)) return false;
  return true;
}

void restore_fds(std::vector<SavedFd> &saved) {
  for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
    if (it->saved >= 0) {
      dup2(it->saved, it->fd);
      close(it->saved);
    } else {
      close(it->fd);
    }
  }
  saved.clear();
}

// Make the applied redirections permanent (`exec < file'): drop the backups
// instead of restoring from them.
void discard_saved_fds(std::vector<SavedFd> &saved) {
  for (const SavedFd &s : saved)
    if (s.saved >= 0) close(s.saved);
  saved.clear();
}

// ---- assignments (scalar, array element, and array literal) --------------

struct Assign {
  std::string name;
  std::optional<std::string> sub;  // subscript, if name[sub]=
  bool append = false;             // +=
  bool is_array = false;           // value is (...)
  std::string value;
};

bool parse_assign(const std::string &w, Assign &a) {
  size_t i = 0;
  while (i < w.size() && (std::isalnum(static_cast<unsigned char>(w[i])) || w[i] == '_')) i++;
  a.name = w.substr(0, i);
  if (a.name.empty()) return false;
  if (i < w.size() && w[i] == '[') {
    size_t s = ++i;
    int d = 1;
    while (i < w.size() && d) {
      if (w[i] == '[') d++;
      else if (w[i] == ']') d--;
      if (d) i++;
    }
    a.sub = w.substr(s, i - s);
    if (i < w.size() && w[i] == ']') i++;
  }
  if (i < w.size() && w[i] == '+') { a.append = true; i++; }
  if (i >= w.size() || w[i] != '=') return false;
  i++;
  a.value = w.substr(i);
  a.is_array = a.value.size() >= 2 && a.value.front() == '(' && a.value.back() == ')';
  return true;
}

std::vector<std::pair<std::optional<std::string>, std::string>>
parse_array_elems(Shell &sh, Expander &ex, const std::string &name, bool integer,
                  bool whole_append, const std::string &parenval) {
  std::vector<std::pair<std::optional<std::string>, std::string>> out;
  std::string inner = parenval.substr(1, parenval.size() - 2);
  for (const Token &t : tokenize(inner)) {
    if (t.type == Tok::Eof) break;
    if (t.type != Tok::Word) continue;
    const std::string &e = t.text;
    if (!e.empty() && e[0] == '[') {
      size_t rb = e.find(']');
      // [sub]=value or [sub]+=value (append to that element).
      bool app = rb != std::string::npos && rb + 2 < e.size() && e[rb + 1] == '+' &&
                 e[rb + 2] == '=';
      bool plain = rb != std::string::npos && rb + 1 < e.size() && e[rb + 1] == '=';
      if (app || plain) {
        std::string sub = ex.expand_no_split(e.substr(1, rb - 1));
        std::string val = ex.expand_no_split(e.substr(rb + (app ? 3 : 2)));
        if (app) {  // resolve against the current element (fresh assign cleared
                    // it, so the base is 0 unless this is a whole-array append)
          std::string base = whole_append ? sh.array_get(name, sh.zsh_subscript(name, sub)) : "0";
          if (integer) {
            bool ok = true;
            val = std::to_string(eval_arith(sh, base, &ok) + eval_arith(sh, val, &ok));
          } else {
            val = base + val;
          }
        }
        out.emplace_back(sub, val);
        continue;
      }
    }
    for (const std::string &f : ex.expand_args({Word{e, t.quoted ? W_QUOTED : 0}}))
      out.emplace_back(std::nullopt, f);
  }
  return out;
}

void apply_array_assign(Shell &sh, Expander &ex, const Assign &a) {
  // An integer-attributed array (`declare -i') evaluates each element value as
  // an arithmetic expression, and `+=' adds rather than string-appends.
  auto vit = sh.vars.find(a.name);
  bool integer = vit != sh.vars.end() && vit->second.integer;
  if (a.sub) {
    // zsh array subscripts are 1-based; translate to the internal 0-based index
    // (a no-op under other personalities / for associative arrays).
    std::string sub = sh.zsh_subscript(a.name, ex.expand_no_split(*a.sub));
    std::string val = ex.expand_assignment(a.value);
    if (integer) {
      bool ok = true;
      long long rhs = eval_arith(sh, val, &ok);
      long long base = a.append ? eval_arith(sh, sh.array_get(a.name, sub), &ok) : 0;
      val = std::to_string(base + rhs);
    } else if (a.append) {
      val = sh.array_get(a.name, sub) + val;
    }
    sh.array_set(a.name, sub, val);
  } else {  // is_array
    bool assoc = sh.vars.count(a.name) && sh.vars[a.name].kind == VarKind::Assoc;
    auto elems = parse_array_elems(sh, ex, a.name, integer, a.append, a.value);
    if (integer)
      for (auto &e : elems) {
        bool ok = true;
        e.second = std::to_string(eval_arith(sh, e.second, &ok));
      }
    sh.array_assign(a.name, elems, a.append, assoc);
  }
}

void gather_pipeline(const Command *c, std::vector<const Command *> &stages) {
  const auto *conn = dynamic_cast<const Connection *>(c);
  if (conn && conn->conn == Connector::Pipe) {
    gather_pipeline(conn->first.get(), stages);
    stages.push_back(conn->second.get());
  } else {
    stages.push_back(c);
  }
}

}  // namespace

namespace {
// A NAME= / NAME+= / NAME[sub]= prefix (an assignment word for a builtin).
bool is_assignment_word_text(const std::string &w) {
  size_t i = 0;
  if (i >= w.size() || !(std::isalpha(static_cast<unsigned char>(w[i])) || w[i] == '_'))
    return false;
  while (i < w.size() && (std::isalnum(static_cast<unsigned char>(w[i])) || w[i] == '_')) i++;
  if (i < w.size() && w[i] == '[') {
    int d = 1;
    i++;
    while (i < w.size() && d) { if (w[i] == '[') d++; else if (w[i] == ']') d--; i++; }
  }
  if (i < w.size() && w[i] == '+') i++;
  return i < w.size() && w[i] == '=';
}

bool is_assignment_builtin(const std::string &cmd) {
  return cmd == "declare" || cmd == "typeset" || cmd == "local" || cmd == "readonly";
}
}  // namespace

void apply_assignment_word(Shell &sh, const std::string &word) {
  Expander ex(sh);
  Assign a;
  if (!parse_assign(word, a)) return;
  if (a.sub || a.is_array)
    apply_array_assign(sh, ex, a);
  else
    sh.set(a.name, ex.expand_assignment(a.value));
}

// Format one %-directive value for `time' (bash's TIMEFORMAT).  A plain form
// prints total seconds with PREC decimals; the long form ("l") prints
// MINUTESmSECONDSs.
static std::string time_value(double sec, int prec, bool longfmt) {
  if (sec < 0) sec = 0;
  char buf[64];
  if (longfmt) {
    long m = static_cast<long>(sec / 60);
    double s = sec - static_cast<double>(m) * 60.0;
    std::snprintf(buf, sizeof buf, "%ldm%.*fs", m, prec, s);
  } else {
    std::snprintf(buf, sizeof buf, "%.*f", prec, sec);
  }
  return buf;
}

// Render a `time' report from the elapsed real/user/sys seconds, following
// bash's TIMEFORMAT grammar (%[p][l]{R|U|S|P}, %%); `time -p' forces the POSIX
// format.  A trailing newline terminates the report.
static std::string time_report(Shell &sh, bool posix, double real, double user,
                               double sys) {
  std::string fmt;
  if (posix) {
    fmt = "real %2R\nuser %2U\nsys %2S";
  } else if (!sh.get_if_set("TIMEFORMAT", fmt)) {
    fmt = "\nreal\t%3lR\nuser\t%3lU\nsys\t%3lS";
  }
  std::string out;
  for (size_t i = 0; i < fmt.size(); i++) {
    if (fmt[i] != '%') { out += fmt[i]; continue; }
    if (++i >= fmt.size()) { out += '%'; break; }
    if (fmt[i] == '%') { out += '%'; continue; }
    int prec = 3;
    if (std::isdigit(static_cast<unsigned char>(fmt[i]))) { prec = fmt[i] - '0'; i++; }
    bool longfmt = false;
    if (i < fmt.size() && fmt[i] == 'l') { longfmt = true; i++; }
    if (i >= fmt.size()) break;
    switch (fmt[i]) {
      case 'R': out += time_value(real, prec, longfmt); break;
      case 'U': out += time_value(user, prec, longfmt); break;
      case 'S': out += time_value(sys, prec, longfmt); break;
      case 'P': {  // %CPU = (user+sys)/real, never long form
        double p = real > 0 ? (user + sys) / real * 100.0 : 0.0;
        out += time_value(p, prec, false);
        break;
      }
      default: out += '%'; out += fmt[i]; break;
    }
  }
  out += '\n';
  return out;
}

int Executor::run(const Command *c) {
  if (!c || unwinding()) return sh_.last_status;

  // `time PIPELINE': measure and report real/user/sys time.  Re-enter run() with
  // the flag suppressed for this node so the body runs exactly once.
  if ((c->flags & CMD_TIME) && c != timed_cmd_) {
    const Command *prev = timed_cmd_;
    timed_cmd_ = c;
    struct timeval w0, w1;
    struct tms t0, t1;
    gettimeofday(&w0, nullptr);
    times(&t0);
    int st = run(c);
    times(&t1);
    gettimeofday(&w1, nullptr);
    timed_cmd_ = prev;
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
    double real = (w1.tv_sec - w0.tv_sec) + (w1.tv_usec - w0.tv_usec) / 1e6;
    double user = static_cast<double>((t1.tms_utime - t0.tms_utime) +
                                      (t1.tms_cutime - t0.tms_cutime)) / hz;
    double sys = static_cast<double>((t1.tms_stime - t0.tms_stime) +
                                     (t1.tms_cstime - t0.tms_cstime)) / hz;
    std::string rep = time_report(sh_, (c->flags & CMD_TIME_POSIX) != 0, real, user, sys);
    std::fputs(rep.c_str(), stderr);
    return st;
  }

  // `-n' (noexec): parse commands but never run them, so a script can be
  // syntax-checked without side effects.  Gating each command here -- rather
  // than skipping the whole tree in run_string -- matches bash: a `set -n'
  // reached mid-script executes (turning noexec on), after which every later
  // command is skipped, and a subsequent `set +n' never runs to turn it back
  // off.  Ignored by interactive shells, exactly as bash does.
  if (sh_.opt_noexec) return sh_.last_status;

  sh_.run_pending_traps();  // deliver any signals received between commands

  // $LINENO / error line for compound commands (run_simple sets its own).
  if (c->line > 0 && !dynamic_cast<const SimpleCommand *>(c))
    sh_.cur_lineno = sh_.lineno_base + c->line;

  // A negated command (`! cmd') never triggers errexit, and neither do the
  // commands nested within it -- bash exempts the entire subtree, so that e.g.
  // the `false' inside `! eval false' does not exit a `set -e' shell.  Suppress
  // errexit for the duration of the negated command's execution.
  struct ErrexitGuard {
    Shell &sh; bool active;
    ErrexitGuard(Shell &s, bool a) : sh(s), active(a) { if (active) sh.errexit_suppress++; }
    ~ErrexitGuard() { if (active) sh.errexit_suppress--; }
  } eg(sh_, (c->flags & CMD_INVERT_RETURN) != 0);

  if (auto *p = dynamic_cast<const SimpleCommand *>(c)) return run_simple(p);
  if (auto *p = dynamic_cast<const Connection *>(c)) return run_connection(p);

  // Compound commands: apply redirects in-process around the body.
  std::vector<SavedFd> saved;
  if (!apply_redirects(sh_, c->redirects, saved)) {
    restore_fds(saved);
    return (sh_.last_status = 1);
  }
  int st = sh_.last_status;
  if (auto *pa = dynamic_cast<const Subshell *>(c)) st = run_subshell(pa);
  else if (auto *pb = dynamic_cast<const Group *>(c)) st = run_group(pb);
  else if (auto *pc = dynamic_cast<const IfCommand *>(c)) st = run_if(pc);
  else if (auto *pd = dynamic_cast<const LoopCommand *>(c)) st = run_loop(pd);
  else if (auto *pe = dynamic_cast<const ForCommand *>(c)) st = run_for(pe);
  else if (auto *pf = dynamic_cast<const CaseCommand *>(c)) st = run_case(pf);
  else if (auto *pg = dynamic_cast<const FunctionDef *>(c)) st = run_funcdef(pg);
  else if (auto *ph = dynamic_cast<const CondCommand *>(c)) st = run_cond(ph);
  else if (auto *pi = dynamic_cast<const ArithCommand *>(c)) st = run_arith(pi);
  restore_fds(saved);
  if (c->flags & CMD_INVERT_RETURN) st = st ? 0 : 1;
  sh_.last_status = st;
  // A compound command (subshell, group, if, loop, ...) that returns non-zero
  // triggers errexit at its own level, e.g. `set -e; (exit 17)' exits the shell.
  // Conditions and negations are already exempted via errexit_suppress / the
  // CMD_INVERT_RETURN guard in run().
  if (sh_.opt_errexit && st != 0 && sh_.errexit_suppress == 0 && !unwinding() &&
      !(c->flags & CMD_INVERT_RETURN)) {
    sh_.exiting = true;
    sh_.exit_status = st;
  }
  return st;
}

int Executor::run_connection(const Connection *c) {
  switch (c->conn) {
    case Connector::Pipe:
      return run_pipeline(c);
    case Connector::And: {
      sh_.errexit_suppress++;
      int st = run(c->first.get());
      sh_.errexit_suppress--;
      if (st == 0 && !unwinding()) st = run(c->second.get());
      return st;
    }
    case Connector::Or: {
      sh_.errexit_suppress++;
      int st = run(c->first.get());
      sh_.errexit_suppress--;
      if (st != 0 && !unwinding()) st = run(c->second.get());
      return st;
    }
    case Connector::Amp: {
      // A bare job spec with `&' (`%1 &') is a synonym for `bg %1': resume the
      // job in the background rather than launching a new child.
      if (auto *sc = dynamic_cast<const SimpleCommand *>(c->first.get())) {
        if (sc->redirects.empty() && sc->words.size() == 1 &&
            !sc->words[0].text.empty() && sc->words[0].text[0] == '%') {
          std::vector<std::string> bgargv = {"bg", sc->words[0].text};
          int status = 0;
          run_builtin(sh_, bgargv, &status);
          return (sh_.last_status = status);
        }
      }
      // Background the first command in its own process group.
      std::string cmd = to_string(c->first.get());
      bool jc = sh_.job_control;
      pid_t pid = fork();
      if (pid == 0) {
        if (jc) setpgid(0, 0);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        sh_.job_control = false;  // background: descendants must not touch the tty
        sh_.subshell_level++;
        Executor ex(sh_);
        int s = ex.run(c->first.get());
        std::fflush(nullptr);
        _exit(s & 0xff);
      }
      if (jc) setpgid(pid, pid);
      sh_.last_bg_pid = pid;
      Shell::Job *j = sh_.add_job(pid, {pid}, cmd, true);
      if (sh_.interactive) std::fprintf(stderr, "[%d] %ld\n", j->id, static_cast<long>(pid));
      if (c->second && !unwinding()) return run(c->second.get());
      return 0;
    }
    case Connector::Semi:
    case Connector::Newline: {
      run(c->first.get());
      if (c->second && !unwinding()) return run(c->second.get());
      return sh_.last_status;
    }
  }
  return 0;
}

int Executor::run_pipeline(const Connection *c) {
  std::vector<const Command *> stages;
  gather_pipeline(c, stages);
  size_t n = stages.size();
  int prev_read = -1;
  std::vector<pid_t> pids;
  long pgid = 0;
  // `shopt -s lastpipe' (with job control off) runs the final stage in the
  // current shell, so its assignments and other side effects persist.
  bool do_lastpipe = n > 1 && !sh_.job_control &&
                     sh_.shopt_opts.count("lastpipe") && sh_.shopt_opts.at("lastpipe");
  bool ran_lastpipe = false;
  int lastpipe_status = 0;
  for (size_t i = 0; i < n; i++) {
    int pipefd[2] = {-1, -1};
    if (i + 1 < n) {
      if (pipe(pipefd) != 0) break;
    }
    if (i == n - 1 && do_lastpipe) {
      // Final stage in-process: read from the previous pipe on stdin, run, then
      // restore stdin.  Not added to `pids' -- it is not a child.
      int saved_in = dup(0);
      // When stdin was closed, the pipe's read end may itself be fd 0; in that
      // case it is already in place, so must not be dup'd-then-closed.
      if (prev_read != -1 && prev_read != 0) {
        dup2(prev_read, 0);
        close(prev_read);
      }
      prev_read = -1;
      lastpipe_status = run(stages[i]);
      ran_lastpipe = true;
      if (saved_in != -1) { dup2(saved_in, 0); close(saved_in); }
      else close(0);  // restore the closed-stdin state
      break;
    }
    pid_t pid = fork();
    if (pid == 0) {
      pid_t me = getpid();
      if (sh_.job_control) setpgid(me, pgid == 0 ? me : static_cast<pid_t>(pgid));
      if (sh_.job_control) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
      }
      if (prev_read != -1) { dup2(prev_read, 0); close(prev_read); }
      if (i + 1 < n) { close(pipefd[0]); dup2(pipefd[1], 1); close(pipefd[1]); }
      sh_.job_control = false;  // pipeline stage: no nested tty control
      // A simple command as a pipeline stage does not raise $BASH_SUBSHELL; a
      // compound one does.  An explicit ( ) subshell counts itself in
      // run_subshell, so don't double-count it here.
      if (!dynamic_cast<const SimpleCommand *>(stages[i]) &&
          !dynamic_cast<const Subshell *>(stages[i]))
        sh_.subshell_level++;
      Executor ex(sh_);
      int s = ex.run(stages[i]);
      std::fflush(nullptr);
      _exit(s & 0xff);
    }
    if (pgid == 0) pgid = pid;
    if (sh_.job_control) setpgid(pid, static_cast<pid_t>(pgid));
    pids.push_back(pid);
    if (prev_read != -1) close(prev_read);
    if (i + 1 < n) { close(pipefd[1]); prev_read = pipefd[0]; }
  }
  if (prev_read != -1) close(prev_read);

  if (sh_.job_control) tcsetpgrp(sh_.job_terminal, static_cast<pid_t>(pgid));
  int last_st = 0, pipefail_st = 0;
  bool any_stopped = false;
  std::vector<int> pstat;  // per-stage status, in pipeline order, for $PIPESTATUS
  for (size_t i = 0; i < pids.size(); i++) {
    int wst = 0;
    waitpid(pids[i], &wst, WUNTRACED);
    if (WIFSTOPPED(wst)) { any_stopped = true; pstat.push_back(128 + WSTOPSIG(wst)); continue; }
    int s = WIFEXITED(wst) ? WEXITSTATUS(wst) : (128 + WTERMSIG(wst));
    pstat.push_back(s);
    if (i == pids.size() - 1) last_st = s;
    if (s != 0) pipefail_st = s;  // track the last (rightmost) non-zero stage
  }
  // The in-process last stage is the rightmost of all: it sets the pipeline
  // status, and (for pipefail) overrides only when it too failed.
  if (ran_lastpipe) {
    pstat.push_back(lastpipe_status);
    last_st = lastpipe_status;
    if (lastpipe_status != 0) pipefail_st = lastpipe_status;
  }
  // Publish $PIPESTATUS (one element per stage, left to right).
  {
    std::vector<std::pair<std::optional<std::string>, std::string>> elems;
    for (int s : pstat) elems.emplace_back(std::nullopt, std::to_string(s));
    sh_.array_assign("PIPESTATUS", elems, false, false);
  }
  // Normally the pipeline's status is the last stage's; under `set -o pipefail'
  // it is the last stage to exit non-zero (0 if all succeeded), so an upstream
  // failure is not masked by a later success.
  int st = sh_.opt_pipefail ? pipefail_st : last_st;
  if (sh_.job_control) tcsetpgrp(sh_.job_terminal, static_cast<pid_t>(sh_.shell_pgid));

  if (any_stopped) {
    std::vector<long> lp(pids.begin(), pids.end());
    Shell::Job *j = sh_.add_job(pgid, lp, to_string(c), false);
    j->stopped = true;
    j->running = false;
    if (sh_.interactive)
      std::fprintf(stderr, "\n[%d]+  Stopped                 %s\n", j->id, j->command.c_str());
    st = 128 + SIGTSTP;
  }

  if (c->flags & CMD_INVERT_RETURN) st = st ? 0 : 1;
  sh_.last_status = st;
  // A pipeline that returns non-zero triggers errexit (e.g. `set -e; true|false').
  // Suppressed contexts (conditions, `&&'/`||' non-final operands, `!') are
  // handled by errexit_suppress and the CMD_INVERT_RETURN guard in run().
  if (sh_.opt_errexit && st != 0 && sh_.errexit_suppress == 0 && !unwinding() &&
      !(c->flags & CMD_INVERT_RETURN)) {
    sh_.exiting = true;
    sh_.exit_status = st;
  }
  return st;
}

int Executor::run_simple(const SimpleCommand *c) {
  if (c->line > 0) sh_.cur_lineno = sh_.lineno_base + c->line;  // $LINENO
  // $BASH_COMMAND tracks the command currently executing (bash sets it before
  // every command, not only inside a DEBUG trap).
  sh_.bash_command = to_string(c);
  // Consume the exec-in-place permission for *this* command up front, so it
  // applies only to a direct external here -- never to commands that a builtin
  // (eval/source) or function invoked by this command goes on to run.
  bool exec_replace = sh_.can_exec_replace;
  sh_.can_exec_replace = false;
  // DEBUG trap: fires before the command, with $BASH_COMMAND set.  If the trap
  // runs `return'/`exit', skip the command and let the unwind propagate.
  if (sh_.traps.count("DEBUG") && !sh_.in_debug_trap) {
    int tst = sh_.run_debug_trap(to_string(c));
    if (unwinding()) return sh_.last_status;
    // shopt -s extdebug: a non-zero DEBUG trap status skips the command.
    auto ed = sh_.shopt_opts.find("extdebug");
    if (tst != 0 && ed != sh_.shopt_opts.end() && ed->second)
      return sh_.last_status;
  }
  Expander ex(sh_);
  // Reap any <(...) / >(...) set up for this command once it (and any function
  // body it invokes) has finished, on every return path.
  struct ProcsubGuard {
    Shell &s;
    size_t base;
    ~ProcsubGuard() { s.reap_procsubs(base); }
  } psg{sh_, sh_.procsubs.size()};
  std::vector<std::pair<std::string, std::string>> assigns;
  std::vector<std::string> argv;
  bool prefix = true;
  // Track command substitutions in the assignment RHS: a pure-assignment
  // command takes the status of the last one (bash), or 0 if there were none.
  sh_.cmdsub_ran = false;
  for (const Word &w : c->words) {
    // Under `set -k' (keyword mode) an assignment-form word ANYWHERE in the
    // command is an assignment, not just those preceding the command name.
    // Checked at run time (the flag can be toggled mid-script) via the word
    // text, since the parser only marks leading words as W_ASSIGNMENT.
    bool assign_here = (prefix && (w.flags & W_ASSIGNMENT)) ||
                       (sh_.opt_keyword && is_assignment_word_text(w.text));
    if (assign_here) {
      Assign a;
      parse_assign(w.text, a);
      if (a.sub || a.is_array) {
        apply_array_assign(sh_, ex, a);  // array element / literal: applied now
      } else {
        auto vit = sh_.vars.find(a.name);
        bool integer = vit != sh_.vars.end() && vit->second.integer;
        // A plain `name=value' / `name+=value' where name is already an array
        // targets element 0 (bash), so read/write that element rather than the
        // scalar field.
        bool is_arr = vit != sh_.vars.end() &&
                      (vit->second.kind == VarKind::Indexed || vit->second.kind == VarKind::Assoc);
        std::string v = ex.expand_assignment(a.value);
        std::string cur = is_arr ? sh_.array_get(a.name, "0") : sh_.get(a.name);
        if (integer) {
          bool ok = true;
          long long rv = eval_arith(sh_, v, &ok);
          if (a.append) rv = eval_arith(sh_, cur, &ok) + rv;
          v = std::to_string(rv);
        } else if (a.append) {
          v = cur + v;
        }
        if (is_arr) sh_.array_set(a.name, "0", v);
        else assigns.emplace_back(a.name, v);
      }
    } else {
      prefix = false;
      // For an assignment builtin (declare/local/readonly/typeset), a name=value
      // argument is an assignment word: pass it through raw so it is neither
      // word-split nor globbed and array literals survive; the builtin expands
      // and parses it itself.
      if (!argv.empty() && is_assignment_builtin(argv[0]) && is_assignment_word_text(w.text))
        argv.push_back(w.text);
      else
        for (const std::string &f : ex.expand_args({w})) argv.push_back(f);
    }
  }

  // A failed arithmetic expansion (bad expression, or assignment to a readonly
  // variable) during word expansion aborts the command with status 1; the
  // shell continues.
  if (sh_.arith_error) {
    sh_.arith_error = false;
    return (sh_.last_status = 1);
  }

  if (sh_.opt_xtrace) {
    std::string line = "+";
    for (const auto &a : assigns) line += " " + a.first + "=" + a.second;
    for (const auto &a : argv) line += " " + a;
    std::fprintf(stderr, "%s\n", line.c_str());
  }

  // `command [-pvV] NAME [args...]': run NAME as a builtin or external, bypassing
  // any shell function of the same name.  The execution case is handled here --
  // not in the `command' builtin -- so NAME reuses the normal redirect / temporary-
  // assignment / job-control path with the already-expanded, correctly-quoted argv.
  // (The builtin re-joined the words into a string and re-parsed them, which
  // corrupted quoting and any embedded shell metacharacters.)  The describe forms
  // -v/-V, and invalid options, are left for the builtin's `command' case, where
  // the name-lookup helpers live: we detect them and simply don't strip.
  bool skip_functions = false;
  while (!argv.empty() && argv[0] == "command") {
    size_t k = 1;
    bool describe = false, bad = false;
    for (; k < argv.size(); k++) {
      const std::string &o = argv[k];
      if (o == "--") { k++; break; }
      if (o.size() < 2 || o[0] != '-') break;
      for (size_t j = 1; j < o.size(); j++) {
        if (o[j] == 'v' || o[j] == 'V') describe = true;
        else if (o[j] == 'p') {  // default PATH; forbidden in a restricted shell
          if (sh_.opt_restricted) {
            std::fprintf(stderr, "%scommand: -p: restricted\n", sh_.err_prefix().c_str());
            return (sh_.last_status = 2);
          }
        }
        else { bad = true; break; }
      }
      if (bad) break;
    }
    if (describe || bad) break;  // -v/-V/invalid: dispatch to the `command' builtin
    argv.erase(argv.begin(), argv.begin() + k);  // strip `command' and its options
    skip_functions = true;
    if (argv.empty()) return (sh_.last_status = 0);  // bare `command'
  }

  // No command word: assignments take effect in the current shell.  The status
  // is that of the last command substitution in the RHS, or 0 if there was none.
  if (argv.empty()) {
    std::vector<SavedFd> saved;
    apply_redirects(sh_, c->redirects, saved);
    for (const auto &a : assigns) sh_.set(a.first, a.second);
    restore_fds(saved);
    int st = sh_.cmdsub_ran ? sh_.last_cmdsub_status : 0;
    if (c->flags & CMD_INVERT_RETURN) st = st ? 0 : 1;  // a bare `!'
    sh_.last_status = st;
    // A failing command substitution in the RHS (`x=$(false)') triggers errexit
    // just like any other command's non-zero status.
    if (sh_.opt_errexit && st != 0 && sh_.errexit_suppress == 0 && !unwinding() &&
        !(c->flags & CMD_INVERT_RETURN)) {
      sh_.exiting = true;
      sh_.exit_status = st;
    }
    return st;
  }

  // A bare job spec in command position (`%1', `%', `%%', `%-', `%name') is a
  // synonym for `fg %spec'.  (The backgrounding form `%spec &' is turned into
  // `bg %spec' in run_connection before we ever get here.)
  if (argv.size() == 1 && argv[0][0] == '%') {
    std::vector<std::string> fgargv = {"fg", argv[0]};
    int status = 0;
    run_builtin(sh_, fgargv, &status);
    return (sh_.last_status = status);
  }

  // A restricted shell forbids a command name containing `/' (a builtin or
  // function of that literal name would already have matched by exact name).
  if (sh_.opt_restricted && argv[0].find('/') != std::string::npos &&
      sh_.functions.find(argv[0]) == sh_.functions.end()) {
    std::fprintf(stderr, "%s%s: restricted: cannot specify `/' in command names\n",
                 sh_.err_prefix().c_str(), argv[0].c_str());
    return (sh_.last_status = 126);
  }

  // Builtins and functions run in-process (with redirects applied/restored).
  auto fit = sh_.functions.find(argv[0]);
  bool is_func = !skip_functions && fit != sh_.functions.end();
  int dummy = 0;
  bool builtin = false;
  {
    // peek: is it a builtin name? run_builtin decides.
    // We apply redirects first, then dispatch.
  }
  (void)dummy;

  std::vector<SavedFd> saved;
  if (!apply_redirects(sh_, c->redirects, saved)) {
    restore_fds(saved);
    return (sh_.last_status = 1);
  }

  // Temporary assignments: set as shell vars for the command and *exported* so
  // child processes spawned by a function see them, then fully restored after
  // (for builtins/functions).  For external commands they go into the env.
  std::vector<std::pair<std::string, std::optional<Variable>>> restore;
  auto apply_temp = [&]() {
    for (const auto &a : assigns) {
      auto it = sh_.vars.find(a.first);
      restore.push_back({a.first,
                         it == sh_.vars.end() ? std::nullopt : std::optional<Variable>(it->second)});
      sh_.set(a.first, a.second);
      sh_.vars[a.first].exported = true;  // visible to the command's children
    }
  };
  auto undo_temp = [&]() {
    for (auto it = restore.rbegin(); it != restore.rend(); ++it) {
      if (it->second) sh_.vars[it->first] = *it->second;
      else sh_.vars.erase(it->first);
    }
    restore.clear();
  };

  int status = 0;
  if (is_func) {
    apply_temp();
    std::vector<std::string> saved_pos = sh_.positional;
    sh_.positional.assign(argv.begin() + 1, argv.end());
    // Record the call for `caller': line of the call site, the function name,
    // and the source.
    sh_.call_stack.push_back({sh_.cur_lineno, argv[0],
                              sh_.shell_name.empty() ? "main" : sh_.shell_name});
    // BASH_SOURCE/FUNCNAME frame: the function runs in the file it was defined
    // in; the call line is where it was invoked in the current file.
    auto fsit = sh_.func_src.find(argv[0]);
    std::string def_src = (fsit != sh_.func_src.end()) ? fsit->second : sh_.current_source();
    sh_.push_src_frame(argv[0], def_src, sh_.cur_lineno, true);
    // BASH_ARGC/BASH_ARGV call-argument stack (extdebug only).  At the outermost
    // call, snapshot the script's positionals for the trailing "main" frame.
    bool pushed_argframe = sh_.opt_extdebug;
    if (pushed_argframe) {
      if (sh_.argframes.empty()) sh_.top_positionals = saved_pos;
      sh_.argframes.emplace_back(argv.begin() + 1, argv.end());
    }
    sh_.push_scope();
    sh_.persona_restore.push_back(std::nullopt);  // for `personality -L' / `emulate -L'
    // Run the body under the lineno_base captured at definition time so $LINENO
    // reports absolute source lines regardless of the caller's input block.
    int saved_lineno_base = sh_.lineno_base;
    auto lbit = sh_.func_lineno_base.find(argv[0]);
    if (lbit != sh_.func_lineno_base.end()) sh_.lineno_base = lbit->second;
    status = run(fit->second);
    sh_.lineno_base = saved_lineno_base;
    if (!sh_.persona_restore.empty()) {
      if (sh_.persona_restore.back()) sh_.set_personality(*sh_.persona_restore.back());
      sh_.persona_restore.pop_back();
    }
    sh_.pop_scope();
    if (pushed_argframe && !sh_.argframes.empty()) sh_.argframes.pop_back();
    sh_.pop_src_frame();
    sh_.call_stack.pop_back();
    sh_.positional = saved_pos;
    if (sh_.returning) { sh_.returning = false; status = sh_.exit_status; }
    // In posix mode, assignments preceding a function call persist.
    if (sh_.opt_posix) restore.clear();
    undo_temp();
  } else if ((apply_temp(), run_builtin(sh_, argv, &status))) {
    // A preceding `VAR=val builtin' applies to the builtin (e.g. IFS=, read),
    // then is restored -- except that posix mode makes assignments before the
    // POSIX special builtins permanent.
    builtin = true;
    if (sh_.opt_posix) {
      static const std::set<std::string> kSpecial = {
          ":",      ".",     "break", "continue", "eval",  "exec",
          "exit",   "export", "readonly", "return", "set", "shift",
          "source", "times", "trap",  "unset"};
      if (kSpecial.count(argv[0])) restore.clear();
    }
    undo_temp();
  } else {
    undo_temp();  // not a builtin after all: the external path sets its own env
    // A command name without `/' that has been hashed (`hash -p', BASH_CMDS)
    // execs the remembered path, as bash does; execvp() still falls back to a
    // $PATH search for the (unhashed) common case.
    std::string exec_file = argv[0];
    if (argv[0].find('/') == std::string::npos) {
      auto h = sh_.hashed.find(argv[0]);
      if (h != sh_.hashed.end()) exec_file = h->second;
    }
    // execvp searches $PATH for a name without `/', or uses a `/' path
    // directly; a hashed name execs its remembered value, and reports that
    // value's name on failure (bash's behavior for `hash'/BASH_CMDS entries).
    auto do_exec = [&](std::vector<char *> &cargv) {
      execvp(exec_file.c_str(), cargv.data());
    };
    // external command.  If we are a disposable subshell child whose sole
    // command this is, exec it in place -- become the command with no second
    // fork/wait (exec_replace was consumed at the top of run_simple).
    if (exec_replace) {
      std::vector<std::string> envs = sh_.environ_block();
      for (const auto &a : assigns) {
        // A temporary assignment overrides any exported value of the same name.
        std::string pre = a.first + "=";
        envs.erase(std::remove_if(envs.begin(), envs.end(),
                                  [&](const std::string &e) { return e.compare(0, pre.size(), pre) == 0; }),
                   envs.end());
        envs.push_back(a.first + "=" + a.second);
      }
      std::vector<char *> envp;
      for (auto &e : envs) envp.push_back(const_cast<char *>(e.c_str()));
      envp.push_back(nullptr);
      environ = envp.data();
      std::vector<char *> cargv;
      for (auto &a : argv) cargv.push_back(const_cast<char *>(a.c_str()));
      cargv.push_back(nullptr);
      std::fflush(nullptr);
      do_exec(cargv);
      if (errno == ENOENT && exec_file.find('/') == std::string::npos)
        std::fprintf(stderr, "%s%s: %s\n", sh_.err_prefix().c_str(), exec_file.c_str(),
                     exec_file == argv[0] ? "command not found" : "not found");
      else
        std::fprintf(stderr, "%s%s: %s\n", sh_.err_prefix().c_str(), exec_file.c_str(), std::strerror(errno));
      _exit(errno == EACCES ? 126 : 127);
    }
    // external command, in its own process group
    pid_t pid = fork();
    if (pid == 0) {
      if (sh_.job_control) setpgid(0, 0);
      if (sh_.job_control) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
      }
      std::vector<std::string> envs = sh_.environ_block();
      for (const auto &a : assigns) {
        // A temporary assignment overrides any exported value of the same name.
        std::string pre = a.first + "=";
        envs.erase(std::remove_if(envs.begin(), envs.end(),
                                  [&](const std::string &e) { return e.compare(0, pre.size(), pre) == 0; }),
                   envs.end());
        envs.push_back(a.first + "=" + a.second);
      }
      std::vector<char *> envp;
      for (auto &e : envs) envp.push_back(const_cast<char *>(e.c_str()));
      envp.push_back(nullptr);
      environ = envp.data();
      std::vector<char *> cargv;
      for (auto &a : argv) cargv.push_back(const_cast<char *>(a.c_str()));
      cargv.push_back(nullptr);
      do_exec(cargv);
      if (errno == ENOENT && exec_file.find('/') == std::string::npos)
        std::fprintf(stderr, "%s%s: %s\n", sh_.err_prefix().c_str(), exec_file.c_str(),
                     exec_file == argv[0] ? "command not found" : "not found");
      else
        std::fprintf(stderr, "%s%s: %s\n", sh_.err_prefix().c_str(), exec_file.c_str(),
                     std::strerror(errno));
      _exit(errno == EACCES ? 126 : 127);
    }
    if (sh_.job_control) setpgid(pid, pid);
    if (sh_.job_control) tcsetpgrp(sh_.job_terminal, pid);
    int wst = 0;
    waitpid(pid, &wst, WUNTRACED);
    if (sh_.job_control) tcsetpgrp(sh_.job_terminal, static_cast<pid_t>(sh_.shell_pgid));
    if (WIFSTOPPED(wst)) {
      std::string cmd;
      for (size_t k = 0; k < argv.size(); k++) { if (k) cmd += ' '; cmd += argv[k]; }
      Shell::Job *j = sh_.add_job(pid, {pid}, cmd, false);
      j->stopped = true;
      j->running = false;
      status = 128 + SIGTSTP;
      if (sh_.interactive)
        std::fprintf(stderr, "\n[%d]+  Stopped                 %s\n", j->id, cmd.c_str());
    } else {
      status = WIFEXITED(wst) ? WEXITSTATUS(wst) : (128 + WTERMSIG(wst));
    }
  }
  (void)builtin;

  // Flush buffered builtin/function output while our redirections are still in
  // effect, so it lands on the right fd and in program order.
  std::fflush(stdout);
  // `exec' makes its redirections permanent in the current shell (this path is
  // only reached when exec had no command word, or its exec failed).
  if (builtin && !argv.empty() && argv[0] == "exec" && status == 0)
    discard_saved_fds(saved);
  else
    restore_fds(saved);
  if (c->flags & CMD_INVERT_RETURN) status = status ? 0 : 1;
  sh_.last_status = status;
  if (sh_.opt_errexit && status != 0 && sh_.errexit_suppress == 0 && !unwinding() &&
      !(c->flags & CMD_INVERT_RETURN)) {
    sh_.exiting = true;
    sh_.exit_status = status;
  }
  return status;
}

int Executor::run_subshell(const Subshell *c) {
  pid_t pid = fork();
  if (pid == 0) {
    sh_.job_control = false;  // the subshell runs as one unit; no nested tty control
    sh_.subshell_level++;
    // (external): a lone simple command can exec in place, no second fork.
    if (dynamic_cast<const SimpleCommand *>(c->body.get())) sh_.can_exec_replace = true;
    Executor ex(sh_);
    int s = ex.run(c->body.get());
    sh_.can_exec_replace = false;
    std::fflush(nullptr);
    _exit(s & 0xff);
  }
  int wst = 0;
  waitpid(pid, &wst, 0);
  return WIFEXITED(wst) ? WEXITSTATUS(wst) : 128;
}

int Executor::run_group(const Group *c) { return run(c->body.get()); }

int Executor::run_if(const IfCommand *c) {
  sh_.errexit_suppress++;
  int cond = run(c->cond.get());
  sh_.errexit_suppress--;
  if (unwinding()) return sh_.last_status;
  if (cond == 0) return run(c->then_part.get());
  if (c->else_part) return run(c->else_part.get());
  return 0;
}

int Executor::run_loop(const LoopCommand *c) {
  int st = 0;
  while (!unwinding()) {
    sh_.errexit_suppress++;
    int cond = run(c->cond.get());
    sh_.errexit_suppress--;
    bool go = c->until ? (cond != 0) : (cond == 0);
    if (!go) break;
    st = run(c->body.get());
    if (sh_.break_count) { sh_.break_count--; break; }
    if (sh_.continue_count) { sh_.continue_count--; continue; }
  }
  return st;
}

int Executor::run_for(const ForCommand *c) {
  int st = 0;
  if (!c->is_arith && !c->var.empty()) {
    // The loop variable must be a valid identifier (validated at execution,
    // as bash does: `NAME: line N: \`x-y': not a valid identifier').
    bool okname = std::isalpha(static_cast<unsigned char>(c->var[0])) || c->var[0] == '_';
    for (char ch : c->var)
      if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')) okname = false;
    if (!okname) {
      std::fprintf(stderr, "%s`%s': not a valid identifier\n", sh_.err_prefix().c_str(),
                   c->var.c_str());
      return (sh_.last_status = 1);
    }
  }
  if (c->is_arith) {
    bool ok = true;
    // The three arithmetic sections undergo parameter/command expansion before
    // evaluation (as bash does), so forms like ${#arr[@]} work inside them.
    Expander aex(sh_);
    auto aeval = [&](const std::string &e) {
      if (e.empty()) return 0LL;
      if (sh_.opt_xtrace) std::fprintf(stderr, "+ (( %s ))\n", e.c_str());
      return static_cast<long long>(eval_arith(sh_, aex.expand_no_split(e), &ok));
    };
    aeval(c->a_init);
    for (;;) {
      if (!c->a_cond.empty() && aeval(c->a_cond) == 0) break;
      st = run(c->body.get());
      if (sh_.break_count) { sh_.break_count--; break; }
      if (sh_.continue_count) sh_.continue_count--;
      if (unwinding()) break;
      aeval(c->a_update);
    }
    return st;
  }

  Expander ex(sh_);
  std::vector<std::string> items;
  if (c->words_present)
    items = ex.expand_args(c->words);
  else
    items = sh_.positional;
  if (sh_.opt_xtrace) {
    std::string line = "+ for " + c->var + " in";
    for (const std::string &it : items) line += " " + it;
    std::fprintf(stderr, "%s\n", line.c_str());
  }
  for (const std::string &item : items) {
    sh_.set(c->var, item);
    st = run(c->body.get());
    if (sh_.break_count) { sh_.break_count--; break; }
    if (sh_.continue_count) { sh_.continue_count--; continue; }
    if (unwinding()) break;
  }
  return st;
}

int Executor::run_case(const CaseCommand *c) {
  Expander ex(sh_);
  std::string word = ex.expand_no_split(c->word.text);
  if (sh_.arith_error) { sh_.arith_error = false; return (sh_.last_status = 1); }
  int st = 0;
  size_t i = 0;
  while (i < c->clauses.size()) {
    const CaseClause &cl = c->clauses[i];
    bool m = false;
    for (const Word &pat : cl.patterns) {
      std::string p = ex.expand_pattern(pat.text);
      if (sh_.arith_error) { sh_.arith_error = false; return (sh_.last_status = 1); }
      std::string pp = p, ww = word;
      if (strmatch(pp.data(), ww.data(), FNM_EXTMATCH) == 0) {
        m = true;
        break;
      }
    }
    if (!m) {
      i++;
      continue;
    }
    st = cl.body ? run(cl.body.get()) : 0;
    if (unwinding()) return st;
    // `;&' falls through into the following clause's body (and keeps falling
    // while those clauses also end in `;&').
    while (c->clauses[i].terminator == 1 && i + 1 < c->clauses.size()) {
      i++;
      st = c->clauses[i].body ? run(c->clauses[i].body.get()) : 0;
      if (unwinding()) return st;
    }
    if (c->clauses[i].terminator == 2) {  // `;;&': resume testing patterns
      i++;
      continue;
    }
    return st;
  }
  return st;
}

int Executor::run_funcdef(const FunctionDef *c) {
  sh_.functions[c->name] = c->body.get();
  sh_.func_src[c->name] = sh_.current_source();  // file it was defined in, for BASH_SOURCE
  sh_.func_lineno_base[c->name] = sh_.lineno_base;  // for $LINENO inside the body
  return 0;
}

int Executor::run_cond(const CondCommand *c) {
  // Minimal [[ ]] evaluation delegated to the test builtin semantics via a
  // small dispatch here.  For now, evaluate simple `a OP b`, unary, and !/&&/||
  // by reusing the expression string tokens is complex; approximate with the
  // test builtin over expanded tokens.
  extern bool eval_cond_expression(Shell &, const std::string &, int *);
  int st = 1;
  eval_cond_expression(sh_, c->expression, &st);
  return st;
}

int Executor::run_arith(const ArithCommand *c) {
  if (sh_.opt_xtrace) std::fprintf(stderr, "+ (( %s ))\n", c->expression.c_str());
  bool ok = true;
  Expander ex(sh_);  // expand ${#arr[@]} etc. before arithmetic evaluation
  long long v = eval_arith(sh_, ex.expand_no_split(c->expression), &ok);
  if (!ok) return 1;
  return v != 0 ? 0 : 1;
}

}  // namespace gnash::core
