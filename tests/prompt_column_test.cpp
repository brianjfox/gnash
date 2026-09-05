// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// prompt_column_test.cpp -- the prompt after output without a trailing newline.
// Runs the built gnash on a pty, plays terminal far enough to answer the
// cursor-position query (CSI 6 n) with the column tracked from the output
// stream, and checks that a partial last line (`printf abc') is NOT erased:
// the prompt is painted right after it, as bash/readline do (`abcP$ ').
// A second run answers nothing and checks the fallback: the prompt starts at
// column 0 and the query is not repeated once it went unanswered.
//
// Usage: prompt_column_test <path-to-gnash>

#include <cctype>
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

// A minimal terminal: tracks the cursor column over the shell's output and,
// when `answer' is set, replies to `ESC [ 6 n' with `ESC [ 1 ; col R'.
struct Term {
  int fd;
  bool answer;
  int col = 0;
  std::string pending;  // bytes of an escape sequence not yet complete

  void feed(const char *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
      char c = buf[i];
      if (!pending.empty()) {
        pending += c;
        if (pending.size() == 2) {
          if (c != '[') pending.clear();
          continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == ';') continue;
        std::string arg = pending.substr(2, pending.size() - 3);
        if (c == 'n' && arg == "6") {
          if (answer) {
            std::string reply = "\033[1;" + std::to_string(col + 1) + "R";
            (void)!write(fd, reply.data(), reply.size());
          }
        } else if (c == 'C') {
          col += arg.empty() ? 1 : std::atoi(arg.c_str());
        } else if (c == 'D') {
          col -= arg.empty() ? 1 : std::atoi(arg.c_str());
        }
        pending.clear();
        continue;
      }
      if (c == '\033') pending = "\033";
      else if (c == '\r' || c == '\n') col = 0;
      else if (c == '\b') col = col > 0 ? col - 1 : 0;
      else if (static_cast<unsigned char>(c) >= ' ') col++;
    }
  }
};

// Read everything the shell writes until it stays quiet for `settle_ms'.
static bool drain(Term &t, std::string &out, int settle_ms) {
  for (;;) {
    struct pollfd p = {t.fd, POLLIN, 0};
    int r = poll(&p, 1, settle_ms);
    if (r <= 0) return true;
    char buf[4096];
    ssize_t n = read(t.fd, buf, sizeof buf);
    if (n <= 0) return false;
    out.append(buf, static_cast<size_t>(n));
    t.feed(buf, static_cast<size_t>(n));
  }
}

static void send(Term &t, const char *s, std::string &out, int settle_ms = 400) {
  (void)!write(t.fd, s, std::strlen(s));
  drain(t, out, settle_ms);
}

// Run one interactive session: `printf abc', then `echo hi', then exit.
static std::string session(const char *gnash, bool answer) {
  int master = -1;
  pid_t pid = forkpty(&master, nullptr, nullptr, nullptr);
  if (pid < 0) {
    perror("forkpty");
    std::exit(2);
  }
  if (pid == 0) {
    setenv("PS1", "P$ ", 1);
    setenv("TERM", "xterm", 1);
    unsetenv("ENV");
    execl(gnash, gnash, "--norc", "-i", static_cast<char *>(nullptr));
    _exit(127);
  }
  Term t{master, answer, 0, {}};
  std::string out;
  drain(t, out, 900);  // initial prompt (the unanswered query times out first)
  send(t, "printf abc\r", out, 900);
  send(t, "echo hi\r", out, 900);
  send(t, "exit\r", out);
  close(master);
  kill(pid, SIGKILL);
  waitpid(pid, nullptr, 0);
  return out;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <gnash>\n", argv[0]);
    return 2;
  }

  // 1. The terminal answers: after `abc' the shell asks where the cursor is,
  //    moves to column 3 and paints the prompt there.  `abc' stays visible.
  {
    std::string out = session(argv[1], /*answer=*/true);
    if (out.find("abc\033[6n") == std::string::npos) {
      std::fprintf(stderr, "FAIL expected a cursor-position query right after `abc'\n%s\n",
                   out.c_str());
      failures++;
    }
    if (out.find("abc\033[6n\r\033[3C\033[KP$ ") == std::string::npos) {
      std::fprintf(stderr, "FAIL expected the prompt painted at column 3 after `abc'\n%s\n",
                   out.c_str());
      failures++;
    }
    if (out.find("abc\r\033[K") != std::string::npos) {
      std::fprintf(stderr, "FAIL the partial line `abc' was erased\n%s\n", out.c_str());
      failures++;
    }
    // Typing on that line keeps repainting from column 3, and the typed
    // command still runs.
    if (out.find("\033[3C\033[KP$ echo hi") == std::string::npos ||
        out.find("\r\nhi\r\n") == std::string::npos) {
      std::fprintf(stderr, "FAIL editing after a partial line did not repaint from column 3\n%s\n",
                   out.c_str());
      failures++;
    }
  }

  // 2. No answer: the prompt falls back to column 0 (erasing the row as
  //    before), and the query is sent only once per session.
  {
    std::string out = session(argv[1], /*answer=*/false);
    size_t queries = gnashtest::count_of(out, "\033[6n");
    if (queries != 1) {
      std::fprintf(stderr, "FAIL expected exactly 1 unanswered query, got %zu\n%s\n", queries,
                   out.c_str());
      failures++;
    }
    if (out.find("abc\r\033[KP$ ") == std::string::npos) {
      std::fprintf(stderr, "FAIL expected the column-0 fallback prompt after `abc'\n%s\n",
                   out.c_str());
      failures++;
    }
    if (out.find("\r\nhi\r\n") == std::string::npos) {
      std::fprintf(stderr, "FAIL `echo hi' did not run in the fallback session\n%s\n",
                   out.c_str());
      failures++;
    }
  }

  if (failures == 0) std::printf("prompt_column_test: all tests passed\n");
  return failures ? 1 : 0;
}
