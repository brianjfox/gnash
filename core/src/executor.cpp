// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// executor.cpp -- execute the command AST.

#include "gnash/core/executor.hpp"

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
parse_array_elems(Expander &ex, const std::string &parenval) {
  std::vector<std::pair<std::optional<std::string>, std::string>> out;
  std::string inner = parenval.substr(1, parenval.size() - 2);
  for (const Token &t : tokenize(inner)) {
    if (t.type == Tok::Eof) break;
    if (t.type != Tok::Word) continue;
    const std::string &e = t.text;
    if (!e.empty() && e[0] == '[') {
      size_t rb = e.find(']');
      if (rb != std::string::npos && rb + 1 < e.size() && e[rb + 1] == '=') {
        out.emplace_back(ex.expand_no_split(e.substr(1, rb - 1)),
                         ex.expand_no_split(e.substr(rb + 2)));
        continue;
      }
    }
    for (const std::string &f : ex.expand_args({Word{e, t.quoted ? W_QUOTED : 0}}))
      out.emplace_back(std::nullopt, f);
  }
  return out;
}

void apply_array_assign(Shell &sh, Expander &ex, const Assign &a) {
  if (a.sub) {
    // zsh array subscripts are 1-based; translate to the internal 0-based index
    // (a no-op under other personalities / for associative arrays).
    std::string sub = sh.zsh_subscript(a.name, ex.expand_no_split(*a.sub));
    std::string val = ex.expand_assignment(a.value);
    if (a.append) val = sh.array_get(a.name, sub) + val;
    sh.array_set(a.name, sub, val);
  } else {  // is_array
    bool assoc = sh.vars.count(a.name) && sh.vars[a.name].kind == VarKind::Assoc;
    sh.array_assign(a.name, parse_array_elems(ex, a.value), a.append, assoc);
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

int Executor::run(const Command *c) {
  if (!c || unwinding()) return sh_.last_status;

  sh_.run_pending_traps();  // deliver any signals received between commands

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
      pid_t pid = fork();
      if (pid == 0) {
        setpgid(0, 0);
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
      setpgid(pid, pid);
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
  for (size_t i = 0; i < n; i++) {
    int pipefd[2] = {-1, -1};
    if (i + 1 < n) {
      if (pipe(pipefd) != 0) break;
    }
    pid_t pid = fork();
    if (pid == 0) {
      pid_t me = getpid();
      setpgid(me, pgid == 0 ? me : static_cast<pid_t>(pgid));
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
    setpgid(pid, static_cast<pid_t>(pgid));
    pids.push_back(pid);
    if (prev_read != -1) close(prev_read);
    if (i + 1 < n) { close(pipefd[1]); prev_read = pipefd[0]; }
  }
  if (prev_read != -1) close(prev_read);

  if (sh_.job_control) tcsetpgrp(sh_.job_terminal, static_cast<pid_t>(pgid));
  int st = 0;
  bool any_stopped = false;
  for (size_t i = 0; i < pids.size(); i++) {
    int wst = 0;
    waitpid(pids[i], &wst, WUNTRACED);
    if (WIFSTOPPED(wst)) any_stopped = true;
    else if (i == pids.size() - 1)
      st = WIFEXITED(wst) ? WEXITSTATUS(wst) : (128 + WTERMSIG(wst));
  }
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
  return st;
}

int Executor::run_simple(const SimpleCommand *c) {
  if (c->line > 0) sh_.cur_lineno = c->line;  // $LINENO
  // Consume the exec-in-place permission for *this* command up front, so it
  // applies only to a direct external here -- never to commands that a builtin
  // (eval/source) or function invoked by this command goes on to run.
  bool exec_replace = sh_.can_exec_replace;
  sh_.can_exec_replace = false;
  // DEBUG trap: fires before the command, with $BASH_COMMAND set.  If the trap
  // runs `return'/`exit', skip the command and let the unwind propagate.
  if (sh_.traps.count("DEBUG") && !sh_.in_debug_trap) {
    sh_.run_debug_trap(to_string(c));
    if (unwinding()) return sh_.last_status;
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
    if (prefix && (w.flags & W_ASSIGNMENT)) {
      Assign a;
      parse_assign(w.text, a);
      if (a.sub || a.is_array) {
        apply_array_assign(sh_, ex, a);  // array element / literal: applied now
      } else {
        auto vit = sh_.vars.find(a.name);
        bool integer = vit != sh_.vars.end() && vit->second.integer;
        std::string v = ex.expand_assignment(a.value);
        if (integer) {
          bool ok = true;
          long long rv = eval_arith(sh_, v, &ok);
          if (a.append) rv = eval_arith(sh_, sh_.get(a.name), &ok) + rv;
          v = std::to_string(rv);
        } else if (a.append) {
          v = sh_.get(a.name) + v;
        }
        assigns.emplace_back(a.name, v);
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

  if (sh_.opt_xtrace) {
    std::string line = "+";
    for (const auto &a : assigns) line += " " + a.first + "=" + a.second;
    for (const auto &a : argv) line += " " + a;
    std::fprintf(stderr, "%s\n", line.c_str());
  }

  // No command word: assignments take effect in the current shell.  The status
  // is that of the last command substitution in the RHS, or 0 if there was none.
  if (argv.empty()) {
    std::vector<SavedFd> saved;
    apply_redirects(sh_, c->redirects, saved);
    for (const auto &a : assigns) sh_.set(a.first, a.second);
    restore_fds(saved);
    return (sh_.last_status = sh_.cmdsub_ran ? sh_.last_cmdsub_status : 0);
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

  // Builtins and functions run in-process (with redirects applied/restored).
  auto fit = sh_.functions.find(argv[0]);
  bool is_func = fit != sh_.functions.end();
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
    std::string saved_arg0 = sh_.arg0;
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
    status = run(fit->second);
    sh_.pop_scope();
    if (pushed_argframe && !sh_.argframes.empty()) sh_.argframes.pop_back();
    sh_.pop_src_frame();
    sh_.call_stack.pop_back();
    sh_.positional = saved_pos;
    sh_.arg0 = saved_arg0;
    if (sh_.returning) { sh_.returning = false; status = sh_.exit_status; }
    undo_temp();
  } else if ((apply_temp(), run_builtin(sh_, argv, &status))) {
    // A preceding `VAR=val builtin' applies to the builtin (e.g. IFS=, read),
    // then is restored.
    builtin = true;
    undo_temp();
  } else {
    undo_temp();  // not a builtin after all: the external path sets its own env
    // external command.  If we are a disposable subshell child whose sole
    // command this is, exec it in place -- become the command with no second
    // fork/wait (exec_replace was consumed at the top of run_simple).
    if (exec_replace) {
      std::vector<std::string> envs = sh_.environ_block();
      for (const auto &a : assigns) envs.push_back(a.first + "=" + a.second);
      std::vector<char *> envp;
      for (auto &e : envs) envp.push_back(const_cast<char *>(e.c_str()));
      envp.push_back(nullptr);
      environ = envp.data();
      std::vector<char *> cargv;
      for (auto &a : argv) cargv.push_back(const_cast<char *>(a.c_str()));
      cargv.push_back(nullptr);
      std::fflush(nullptr);
      execvp(cargv[0], cargv.data());
      if (errno == ENOENT && argv[0].find('/') == std::string::npos)
        std::fprintf(stderr, "%s%s: command not found\n", sh_.err_prefix().c_str(), argv[0].c_str());
      else
        std::fprintf(stderr, "%s%s: %s\n", sh_.err_prefix().c_str(), argv[0].c_str(), std::strerror(errno));
      _exit(errno == EACCES ? 126 : 127);
    }
    // external command, in its own process group
    pid_t pid = fork();
    if (pid == 0) {
      setpgid(0, 0);
      if (sh_.job_control) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
      }
      std::vector<std::string> envs = sh_.environ_block();
      for (const auto &a : assigns) envs.push_back(a.first + "=" + a.second);
      std::vector<char *> envp;
      for (auto &e : envs) envp.push_back(const_cast<char *>(e.c_str()));
      envp.push_back(nullptr);
      environ = envp.data();
      std::vector<char *> cargv;
      for (auto &a : argv) cargv.push_back(const_cast<char *>(a.c_str()));
      cargv.push_back(nullptr);
      execvp(cargv[0], cargv.data());
      if (errno == ENOENT && argv[0].find('/') == std::string::npos)
        std::fprintf(stderr, "%s%s: command not found\n", sh_.err_prefix().c_str(),
                     argv[0].c_str());
      else
        std::fprintf(stderr, "%s%s: %s\n", sh_.err_prefix().c_str(), argv[0].c_str(),
                     std::strerror(errno));
      _exit(errno == EACCES ? 126 : 127);
    }
    setpgid(pid, pid);
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
  if (c->is_arith) {
    bool ok = true;
    // The three arithmetic sections undergo parameter/command expansion before
    // evaluation (as bash does), so forms like ${#arr[@]} work inside them.
    Expander aex(sh_);
    auto aeval = [&](const std::string &e) {
      if (e.empty()) return 0LL;
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
  for (const CaseClause &cl : c->clauses) {
    for (const Word &pat : cl.patterns) {
      std::string p = ex.expand_no_split(pat.text);
      std::string pp = p, ww = word;
      if (strmatch(pp.data(), ww.data(), FNM_EXTMATCH) == 0) {
        int st = cl.body ? run(cl.body.get()) : 0;
        return st;
      }
    }
  }
  return 0;
}

int Executor::run_funcdef(const FunctionDef *c) {
  sh_.functions[c->name] = c->body.get();
  sh_.func_src[c->name] = sh_.current_source();  // file it was defined in, for BASH_SOURCE
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
  bool ok = true;
  Expander ex(sh_);  // expand ${#arr[@]} etc. before arithmetic evaluation
  long long v = eval_arith(sh_, ex.expand_no_split(c->expression), &ok);
  if (!ok) return 1;
  return v != 0 ? 0 : 1;
}

}  // namespace gnash::core
