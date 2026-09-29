// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// tty_restore_test.cpp -- terminal state after a foreground job ends.
// Runs the built gnash on a pty and checks bash's wait_for() rule: a job
// killed by a signal gets the shell's saved tty settings put back (a program
// in raw mode had no chance to clean up), while a job that exits normally
// after changing the tty (`stty -echo') keeps its change.
//
// Usage: tty_restore_test <path-to-gnash>

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

#include "testcheck.hpp"

using gnashtest::failures;

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

// Ask the shell whether ECHO is on for its terminal, as seen by a command run
// from the prompt (readline has already restored its own changes by then).
// The result marker is spelled so that the echoed command line itself never
// contains it -- only the command's output does.
static std::string echo_state(int master) {
  std::string out;
  send(master,
       "r=unknown; case \" $(stty -a) \" in *' -echo '*) r=off;; *' echo '*) r=on;; esac; "
       "echo \"RES\"\"ULT=$r\"\r",
       out);
  drain(master, out, 300);
  if (out.find("RESULT=off") != std::string::npos) return "off";
  if (out.find("RESULT=on") != std::string::npos) return "on";
  return "unknown: " + out;
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

  std::string st = echo_state(master);
  if (st != "on") {
    std::fprintf(stderr, "FAIL expected echo on at startup, got %s\n", st.c_str());
    failures++;
  }

  // A foreground job that turns echo off and then dies from a signal: the
  // shell puts its saved settings back before the next prompt.
  send(master, "sh -c 'stty -echo; kill -INT $$'\r", out);
  drain(master, out, 600);
  st = echo_state(master);
  if (st != "on") {
    std::fprintf(stderr, "FAIL expected echo restored after a job killed by SIGINT, got %s\n",
                 st.c_str());
    failures++;
  }

  // The same for a pipeline whose last stage is the one killed.
  send(master, "true | sh -c 'stty -echo; kill -TERM $$'\r", out);
  drain(master, out, 600);
  st = echo_state(master);
  if (st != "on") {
    std::fprintf(stderr, "FAIL expected echo restored after a pipeline killed by SIGTERM, got %s\n",
                 st.c_str());
    failures++;
  }

  // A job that exits normally after `stty -echo' meant it: the change stays,
  // and becomes the state a later signal-killed job is restored to.
  send(master, "stty -echo\r", out);
  drain(master, out, 300);
  st = echo_state(master);
  if (st != "off") {
    std::fprintf(stderr, "FAIL expected `stty -echo' to persist after a normal exit, got %s\n",
                 st.c_str());
    failures++;
  }
  send(master, "sh -c 'stty echo; kill -INT $$'\r", out);
  drain(master, out, 600);
  st = echo_state(master);
  if (st != "off") {
    std::fprintf(stderr, "FAIL expected the saved (echo off) state back after SIGINT, got %s\n",
                 st.c_str());
    failures++;
  }

  send(master, "stty echo\r", out);
  send(master, "exit\r", out);
  close(master);
  kill(pid, SIGKILL);
  waitpid(pid, nullptr, 0);

  if (failures == 0) std::printf("tty_restore_test: all tests passed\n");
  return failures ? 1 : 0;
}
