// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// jobs.cpp -- job control: process groups, controlling terminal, job table.
//
// Follows the structure of bash 5.3 jobs.c at a high level: each pipeline (or
// background command) runs in its own process group; the shell hands the
// terminal to a foreground job and reclaims it afterward, and tracks jobs so
// `jobs'/`fg'/`bg'/`wait' can act on them.

#include "gnash/core/shell.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace gnash::core {

void Shell::init_job_control(bool interactive_shell) {
  interactive = interactive_shell;
  // A shell must reap its own children: reset SIGCHLD to default in case we
  // inherited SIG_IGN (which auto-reaps and makes waitpid() report ECHILD).
  signal(SIGCHLD, SIG_DFL);
  if (interactive_shell && isatty(STDIN_FILENO)) {
    job_terminal = STDIN_FILENO;
    // Wait until we are in the foreground process group.
    while (tcgetpgrp(job_terminal) != (shell_pgid = getpgrp()))
      kill(-shell_pgid, SIGTTIN);
    // The shell ignores the job-control signals so they reach the job instead.
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    // Put ourselves in our own process group and take the terminal.
    shell_pgid = getpid();
    if (setpgid(shell_pgid, shell_pgid) < 0 && errno != EPERM) { /* ignore */ }
    tcsetpgrp(job_terminal, shell_pgid);
    job_control = true;
  } else {
    static bool warned = false;
    if (interactive_shell && !warned) {
      warned = true;
      // Forced interactive (-i) without a terminal, as bash reports it (once).
      std::fprintf(stderr, "%s: cannot set terminal process group (%d): %s\n",
                   shell_name.c_str(), static_cast<int>(getpgrp()),
                   std::strerror(ENOTTY));
      std::fprintf(stderr, "%s: no job control in this shell\n", shell_name.c_str());
    }
    job_control = false;
    shell_pgid = getpgrp();
  }
}

Shell::Job *Shell::add_job(long pgid, const std::vector<long> &pids, const std::string &cmd,
                           bool background) {
  Job j;
  // Job numbers are one above the highest currently-tracked job, so the count
  // starts back at 1 whenever the job table has drained (matching bash).
  int maxid = 0;
  for (const auto &e : jobs) maxid = std::max(maxid, e.id);
  j.id = maxid + 1;
  j.pgid = pgid;
  j.pids = pids;
  j.command = cmd;
  j.background = background;
  j.running = true;
  jobs.push_back(j);
  return &jobs.back();
}

Shell::Job *Shell::job_by_spec(const std::string &spec) {
  if (spec.empty()) {
    for (auto it = jobs.rbegin(); it != jobs.rend(); ++it)
      if (!it->done) return &*it;
    return nullptr;
  }
  if (spec[0] == '%') {
    std::string s = spec.substr(1);
    if (s.empty() || s == "%" || s == "+" || s == "-") {
      // current (or previous for `-')
      std::vector<Job *> live;
      for (auto &j : jobs)
        if (!j.done) live.push_back(&j);
      if (live.empty()) return nullptr;
      if (s == "-") return live.size() >= 2 ? live[live.size() - 2] : live.back();
      return live.back();
    }
    if (std::isdigit(static_cast<unsigned char>(s[0]))) {
      int id = std::atoi(s.c_str());
      for (auto &j : jobs)
        if (j.id == id) return &j;
      return nullptr;
    }
    for (auto &j : jobs)  // prefix match on command
      if (!j.done && j.command.rfind(s, 0) == 0) return &j;
    return nullptr;
  }
  long pid = std::atol(spec.c_str());
  for (auto &j : jobs)
    for (long p : j.pids)
      if (p == pid) return &j;
  return nullptr;
}

namespace {
// Wait for a single job to stop or complete; returns the last member's status.
int wait_job(Shell::Job &j) {
  int status = 0;
  for (long pid : j.pids) {
    int st = 0;
    if (waitpid(static_cast<pid_t>(pid), &st, WUNTRACED) < 0) continue;
    if (WIFSTOPPED(st)) {
      j.stopped = true;
      j.running = false;
      return 128 + WSTOPSIG(st);
    }
    status = WIFEXITED(st) ? WEXITSTATUS(st) : (128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0));
  }
  j.done = true;
  j.running = false;
  j.status = status;
  return status;
}
}  // namespace

int Shell::foreground_job(Job &j, bool cont) {
  if (job_control) tcsetpgrp(job_terminal, static_cast<pid_t>(j.pgid));
  if (cont) {
    kill(static_cast<pid_t>(-j.pgid), SIGCONT);
    j.stopped = false;
    j.running = true;
  }
  int st = wait_job(j);
  if (job_control) {
    tcsetpgrp(job_terminal, static_cast<pid_t>(shell_pgid));
  }
  last_status = st;
  return st;
}

void Shell::background_job(Job &j, bool cont) {
  if (cont) {
    kill(static_cast<pid_t>(-j.pgid), SIGCONT);
    j.stopped = false;
    j.running = true;
  }
  j.background = true;
}

int Shell::wait_for_pid(long pid) {
  int st = 0;
  if (waitpid(static_cast<pid_t>(pid), &st, 0) < 0) return 127;
  note_child_reaped();
  int rc = WIFEXITED(st) ? WEXITSTATUS(st)
                         : (WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 128);
  for (auto &j : jobs)
    for (long p : j.pids)
      if (p == pid) { j.done = true; j.running = false; j.status = rc; }
  return rc;
}

int Shell::wait_all() {
  int st = 0;
  for (auto &j : jobs) {
    if (!j.done) { st = wait_job(j); if (j.done) for (size_t k = 0; k < j.pids.size(); k++) note_child_reaped(); }
  }
  int wst;
  while (waitpid(-1, &wst, 0) > 0) note_child_reaped();  // reap any stragglers
  return st;
}

// Reap any finished/stopped children (non-blocking) and update the job table.
// Returns true if there is at least one job event not yet reported to the user.
bool Shell::check_job_events() {
  int wst = 0;
  pid_t pid;
  while ((pid = waitpid(-1, &wst, WNOHANG | WUNTRACED)) > 0) {
    for (auto &j : jobs) {
      for (long p : j.pids) {
        if (p != pid) continue;
        if (WIFSTOPPED(wst)) { j.stopped = true; j.running = false; }
        else {
          j.done = true;
          j.running = false;
          j.status = WIFEXITED(wst) ? WEXITSTATUS(wst) : 128;
        }
      }
    }
    if (!WIFSTOPPED(wst)) note_child_reaped();  // a background child terminated
  }
  for (const Job &j : jobs)
    if ((j.done || j.stopped) && !j.notified) return true;
  return false;
}

// Print pending "[n]+ Done/Stopped" notices, mark them reported, and drop the
// jobs that have finished.  Callers that share the terminal with readline are
// responsible for clearing/redisplaying the input line around this.
void Shell::emit_job_notices() {
  for (auto &j : jobs) {
    if (j.done && !j.notified) {
      std::fprintf(stderr, "[%d]+  Done                    %s\n", j.id, j.command.c_str());
      j.notified = true;
    } else if (j.stopped && !j.notified) {
      std::fprintf(stderr, "[%d]+  Stopped                 %s\n", j.id, j.command.c_str());
      j.notified = true;
    }
  }
  jobs.erase(std::remove_if(jobs.begin(), jobs.end(),
                            [](const Job &j) { return j.done && j.notified; }),
             jobs.end());
}

void Shell::reap_jobs(bool notify) {
  bool pending = check_job_events();
  if (notify && interactive && pending) {
    emit_job_notices();
  } else {
    // Drop fully-notified/done jobs even when not reporting.
    jobs.erase(std::remove_if(jobs.begin(), jobs.end(),
                              [](const Job &j) { return j.done && j.notified; }),
               jobs.end());
  }
}

void Shell::print_jobs() {
  reap_jobs(false);
  for (const Job &j : jobs) {
    const char *state = j.done ? "Done" : (j.stopped ? "Stopped" : "Running");
    std::printf("[%d]%c  %-22s %s\n", j.id, '+', state, j.command.c_str());
  }
}

}  // namespace gnash::core
