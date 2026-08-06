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
  // Record whether job control was active when the job started: bash's
  // J_JOBCONTROL.  `fg'/`bg' only operate on such jobs; one started with
  // monitor off reports "started without job control" even after `set -m'.
  j.monitored = job_control || opt_monitor;
  jobs.push_back(j);
  // A newly-created background job becomes the current job (bash: reset_current
  // after stop_pipeline for an async job).
  if (background) reset_current();
  return &jobs.back();
}

namespace {
// The most recent (highest-id) live job in the wanted run state whose id is
// below `below' (0 = no bound), mirroring bash's most_recent_job_in_state.
int most_recent_in_state(const std::vector<Shell::Job> &jobs, int below,
                         bool want_stopped) {
  int best = 0;
  for (const auto &j : jobs) {
    if (j.done) continue;
    if (below > 0 && j.id >= below) continue;
    if (j.stopped == want_stopped && j.id > best) best = j.id;
  }
  return best;
}
}  // namespace

// Make ID the current job and choose a useful previous job (bash set_current_job).
void Shell::set_current_job(int id) {
  auto find = [&](int i) -> Job * {
    for (auto &j : jobs) if (j.id == i && !j.done) return &j;
    return nullptr;
  };
  if (j_current != id) { j_previous = j_current; j_current = id; }
  // First choice for previous: the old current job, if it is stopped.
  Job *p = find(j_previous);
  if (j_previous != j_current && p && p->stopped) return;
  // Second choice: newest stopped job older than the current one.
  Job *c = find(j_current);
  if (c && c->stopped) {
    int cand = most_recent_in_state(jobs, j_current, true);
    if (cand) { j_previous = cand; return; }
  }
  // Otherwise the newest running job (older than current if current is running).
  int cand = (c && !c->stopped) ? most_recent_in_state(jobs, j_current, false)
                                : most_recent_in_state(jobs, 0, false);
  j_previous = cand ? cand : j_current;
}

// Recompute current/previous after jobs change (bash reset_current).
void Shell::reset_current() {
  auto find = [&](int i) -> Job * {
    for (auto &j : jobs) if (j.id == i && !j.done) return &j;
    return nullptr;
  };
  int cand = 0;
  Job *cur = find(j_current);
  if (cur && cur->stopped) {
    cand = j_current;  // a stopped current job stays current
  } else {
    Job *prev = find(j_previous);
    if (prev && prev->stopped) cand = j_previous;               // first: previous, if stopped
    if (!cand) cand = most_recent_in_state(jobs, 0, true);      // second: newest stopped
    if (!cand) cand = most_recent_in_state(jobs, 0, false);     // third: newest running
  }
  if (cand) set_current_job(cand);
  else { j_current = j_previous = 0; }
}

void Shell::remove_jobs_if(const std::function<bool(const Job &)> &pred) {
  jobs.erase(std::remove_if(jobs.begin(), jobs.end(), pred), jobs.end());
  reset_current();
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
      int want = (s == "-") ? j_previous : j_current;  // current (or previous for `-')
      if (want == 0) return nullptr;
      for (auto &j : jobs)
        if (j.id == want && !j.done) return &j;
      return nullptr;
    }
    if (std::isdigit(static_cast<unsigned char>(s[0]))) {
      int id = std::atoi(s.c_str());
      for (auto &j : jobs)
        if (j.id == id) return &j;
      return nullptr;
    }
    if (s[0] == '?') {  // %?str: the job whose command CONTAINS str
      std::string sub = s.substr(1);
      for (auto &j : jobs)
        if (!j.done && j.command.find(sub) != std::string::npos) return &j;
      return nullptr;
    }
    for (auto &j : jobs)  // %str: the job whose command STARTS with str
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
    // With a job-control terminal the job has its own process group; without
    // one (set -m in a script) its members share the shell's group, so signal
    // the member pids directly rather than the group.
    if (job_control) kill(static_cast<pid_t>(-j.pgid), SIGCONT);
    else for (long p : j.pids) kill(static_cast<pid_t>(p), SIGCONT);
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
    if (job_control) kill(static_cast<pid_t>(-j.pgid), SIGCONT);
    else for (long p : j.pids) kill(static_cast<pid_t>(p), SIGCONT);
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

int Shell::wait_next(const std::vector<int> &ids, long *finished_pid, bool *found) {
  *found = false;
  if (finished_pid) *finished_pid = -1;
  auto is_target = [&](const Job &j) {
    return ids.empty() || std::find(ids.begin(), ids.end(), j.id) != ids.end();
  };
  // No matching job at all: bash's `wait -n' returns 127 ("no such job").
  bool any = false;
  for (const auto &j : jobs)
    if (is_target(j)) { any = true; break; }
  if (!any) return 127;

  // Consume an already-finished target immediately (it terminated before this
  // `wait -n', e.g. reaped by a prior non-blocking check).
  for (size_t k = 0; k < jobs.size(); k++) {
    if (is_target(jobs[k]) && jobs[k].done) {
      int rc = jobs[k].status;
      if (finished_pid && !jobs[k].pids.empty()) *finished_pid = jobs[k].pids.back();
      *found = true;
      jobs.erase(jobs.begin() + k);
      reset_current();
      return rc;
    }
  }

  // Otherwise block for children to change state, updating the job table, until
  // a target job has fully terminated.  A job's status is its last member's
  // (pipeline semantics); intermediate members are reaped as they exit.
  for (;;) {
    int wst = 0;
    pid_t pid = waitpid(-1, &wst, WUNTRACED);
    if (pid <= 0) {
      if (errno == EINTR) continue;
      return 127;  // no more children to wait for
    }
    Job *owner = nullptr;
    for (auto &j : jobs) {
      for (long p : j.pids)
        if (p == pid) { owner = &j; break; }
      if (owner) break;
    }
    if (WIFSTOPPED(wst)) {
      if (owner) { owner->stopped = true; owner->running = false; }
      continue;
    }
    note_child_reaped();  // a background child terminated -> pending SIGCHLD trap
    if (!owner) continue;  // an untracked child (e.g. a command-substitution straggler)
    int rc = WIFEXITED(wst) ? WEXITSTATUS(wst)
                            : 128 + (WIFSIGNALED(wst) ? WTERMSIG(wst) : 0);
    // The job is done once its final member exits; that member's status is the
    // job's exit status.
    if (pid == owner->pids.back()) {
      owner->done = true;
      owner->running = false;
      owner->status = rc;
      if (is_target(*owner)) {
        if (finished_pid) *finished_pid = owner->pids.back();
        *found = true;
        int r = owner->status;
        jobs.erase(jobs.begin() + (owner - &jobs[0]));
        reset_current();
        return r;
      }
    }
  }
}

int Shell::wait_all() {
  // Block until every background job has terminated, reaping whichever child
  // changes state first (not job-by-job in order, so one job finishing early
  // is observed immediately).  A trapped signal interrupts the wait: bash's
  // `wait' returns 128+signum, then the trap runs (jobs9).
  begin_interruptible_wait();
  for (;;) {
    bool running = false;
    for (const auto &j : jobs)
      if (!j.done && !j.stopped) { running = true; break; }
    if (!running) break;

    int wst = 0;
    pid_t pid = waitpid(-1, &wst, WUNTRACED);
    if (pid < 0) {
      if (errno == EINTR) {
        if (int s = pending_trapped_signal()) { end_interruptible_wait(); return 128 + s; }
        continue;
      }
      break;  // ECHILD: no children left despite a job marked running (already reaped)
    }
    if (pid == 0) continue;

    Job *owner = nullptr;
    for (auto &j : jobs)
      if (std::find(j.pids.begin(), j.pids.end(), static_cast<long>(pid)) != j.pids.end()) {
        owner = &j;
        break;
      }
    if (WIFSTOPPED(wst)) {
      if (owner) { owner->stopped = true; owner->running = false; }
      continue;
    }
    note_child_reaped();
    if (!owner) continue;  // an untracked child (command-substitution straggler)
    int rc = WIFEXITED(wst) ? WEXITSTATUS(wst)
                            : 128 + (WIFSIGNALED(wst) ? WTERMSIG(wst) : 0);
    if (static_cast<long>(pid) == owner->pids.back()) {
      owner->done = true;
      owner->running = false;
      owner->status = rc;
    }
  }
  int wst;
  while (waitpid(-1, &wst, WNOHANG) > 0) note_child_reaped();  // reap exited stragglers
  end_interruptible_wait();
  // bash's `wait' with no operands removes every terminated job it reaped.
  remove_jobs_if([](const Job &j) { return j.done; });
  // `wait' with no operands always returns 0 (`f1 & wait' is 0 even though
  // f1 exited 5); only a trap interruption above returns 128+signal.
  return 0;
}

// Reap any finished/stopped children (non-blocking) and update the job table.
// Returns true if there is at least one job event not yet reported to the user.
bool Shell::check_job_events() {
  int wst = 0;
  pid_t pid;
  int last_stopped = 0;
  while ((pid = waitpid(-1, &wst, WNOHANG | WUNTRACED)) > 0) {
    for (auto &j : jobs) {
      for (long p : j.pids) {
        if (p != pid) continue;
        if (WIFSTOPPED(wst)) { j.stopped = true; j.running = false; last_stopped = j.id; }
        else {
          j.done = true;
          j.running = false;
          j.status = WIFEXITED(wst) ? WEXITSTATUS(wst) : 128;
        }
      }
    }
    if (!WIFSTOPPED(wst)) note_child_reaped();  // a background child terminated
  }
  // A job that stopped becomes the current job.  A job that merely terminated
  // does NOT recompute current/previous: bash's set_job_status_and_cleanup only
  // bumps call_set_current on a stop, so current keeps pointing at a dead job
  // (still marked `+') until the job is actually removed from the table.
  if (last_stopped) set_current_job(last_stopped);
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

void Shell::print_jobs(const std::string &spec, bool lflag, bool pflag, bool rflag,
                       bool sflag) {
  reap_jobs(false);
  // A jobspec restricts the listing to that one job.
  int only = -1;
  if (!spec.empty()) {
    Job *j = job_by_spec(spec);
    if (!j) {
      std::fprintf(stderr, "%sjobs: %s: no such job\n", err_prefix().c_str(), spec.c_str());
      return;
    }
    only = j->id;
  }
  // bash marks the current job `+' and the previous job `-' (others a space).
  int cur = j_current;
  int prev = j_previous;
  for (const Job &j : jobs) {
    if (only != -1 && j.id != only) continue;
    if (rflag && (j.done || j.stopped)) continue;  // -r: running jobs only
    if (sflag && !j.stopped) continue;             // -s: stopped jobs only
    if (pflag) {  // -p: process-group id only (the job's leader pid)
      std::printf("%ld\n", j.pgid);
      continue;
    }
    const char *state = j.done ? "Done" : (j.stopped ? "Stopped" : "Running");
    char mark = j.id == cur ? '+' : j.id == prev ? '-' : ' ';
    // A running background job is shown with a trailing ` &' (bash: RUNNING &&
    // not foreground); the status field is padded to LONGEST_SIGNAL_DESC (27).
    const char *amp = (j.background && !j.done && !j.stopped) ? " &" : "";
    if (lflag)  // -l: include the process-group id before the state
      std::printf("[%d]%c %ld %-27s%s%s\n", j.id, mark, j.pgid, state, j.command.c_str(), amp);
    else
      std::printf("[%d]%c  %-27s%s%s\n", j.id, mark, state, j.command.c_str(), amp);
  }
  // Reporting a terminated job consumes it (bash removes DEADJOB entries once
  // listed).  Only when listing the whole table, not a single -r/-s filter view.
  if (only == -1 && !rflag && !sflag && !pflag)
    remove_jobs_if([](const Job &j) { return j.done; });
}

}  // namespace gnash::core
