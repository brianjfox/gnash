// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// job_notify_test.cpp -- interactive job-control notification behavior.
// Runs the built gnash on a pty, suspends a foreground command with ^Z, and
// checks the resulting job notices against bash's behavior.
//
// Usage: job_notify_test <path-to-gnash>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

static int failures = 0;

// Read everything the shell writes to the pty until it stays quiet for
// `settle_ms', appending to `out'.  Returns false once the child hangs up.
static bool drain(int fd, std::string &out, int settle_ms) {
  for (;;) {
    struct pollfd p = {fd, POLLIN, 0};
    int r = poll(&p, 1, settle_ms);
    if (r <= 0) return true;  // quiet: caller may continue
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof buf);
    if (n <= 0) return false;  // EIO/EOF: shell exited
    out.append(buf, static_cast<size_t>(n));
  }
}

static void send(int fd, const char *s, std::string &out) {
  (void)!write(fd, s, std::strlen(s));
  drain(fd, out, 400);
}

static size_t count_of(const std::string &hay, const std::string &needle) {
  size_t n = 0;
  for (size_t at = hay.find(needle); at != std::string::npos;
       at = hay.find(needle, at + needle.size()))
    n++;
  return n;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <gnash>\n", argv[0]);
    return 2;
  }

  int master = -1;
  pid_t pid = forkpty(&master, nullptr, nullptr, nullptr);
  if (pid < 0) {
    perror("forkpty");
    return 2;
  }
  if (pid == 0) {
    setenv("PS1", "T> ", 1);
    unsetenv("ENV");
    execl(argv[1], argv[1], "-i", static_cast<char *>(nullptr));
    _exit(127);
  }

  std::string out;
  drain(master, out, 600);  // initial prompt

  send(master, "sleep 300\r", out);
  send(master, "\x1a", out);  // ^Z: stop the foreground job
  drain(master, out, 600);    // let the next prompt (and any late notice) land

  // The stop of a foreground job is reported exactly once (bash prints one
  // "[1]+  Stopped" line, not one at stop time and another before the prompt).
  size_t notices = count_of(out, "Stopped");
  if (notices != 1) {
    std::fprintf(stderr, "FAIL expected exactly 1 Stopped notice after ^Z, got %zu\n%s\n",
                 notices, out.c_str());
    failures++;
  }

  // Clean up: kill the stopped sleep and leave the shell.
  send(master, "kill -9 %1\r", out);
  send(master, "exit\r", out);
  send(master, "exit\r", out);
  close(master);
  kill(pid, SIGKILL);
  waitpid(pid, nullptr, 0);

  if (failures == 0) std::printf("job_notify_test: all tests passed\n");
  return failures ? 1 : 0;
}
