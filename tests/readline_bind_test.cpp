// readline_bind_test.cpp -- inputrc parsing/binding and vi mode.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "readline/readline.h"

static int failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                          \
    }                                                                      \
  } while (0)

static char *run(const std::string &input) {
  FILE *f = std::tmpfile();
  if (input.size()) std::fwrite(input.data(), 1, input.size(), f);
  std::rewind(f);
  rl_instream = f;
  rl_outstream = std::fopen("/dev/null", "w");
  char *r = readline("");
  std::fclose(f);
  if (rl_outstream) std::fclose(rl_outstream);
  rl_instream = nullptr;
  rl_outstream = nullptr;
  return r;
}

static void expect(const std::string &in, const char *want) {
  char *r = run(in);
  if (r == nullptr || std::strcmp(r, want) != 0) {
    std::fprintf(stderr, "FAIL run: got \"%s\", wanted \"%s\"\n", r ? r : "(null)", want);
    failures++;
  }
  std::free(r);
}

int main() {
  rl_initialize();

  // ---- rl_named_function ----
  CHECK(rl_named_function("beginning-of-line") == rl_beg_of_line);
  CHECK(rl_named_function("kill-word") == rl_kill_word);
  CHECK(rl_named_function("no-such-function") == nullptr);

  // ---- rl_parse_and_bind: bind C-o to end-of-line ----
  {
    char line[] = "\"\\C-o\": end-of-line";
    CHECK(rl_parse_and_bind(line) == 0);
  }
  // "ab" C-a (to start) then C-o (our binding -> end-of-line) then 'Z' -> "abZ"
  expect("ab\x01\x0f"
         "Z\n",
         "abZ");

  // ---- bind a multi-key sequence: ESC [ Z (back-tab) -> beginning-of-line ----
  {
    char line[] = "\"\\e[Z\": beginning-of-line";
    CHECK(rl_parse_and_bind(line) == 0);
  }
  expect("abc\x1b[Z"
         "X\n",
         "Xabc");

  // ---- vi mode via `set editing-mode vi` ----
  {
    char line[] = "set editing-mode vi";
    rl_parse_and_bind(line);
  }
  CHECK(rl_editing_mode == 0);

  // In vi: type "hello", ESC (command mode, cursor on 'o'), '0' (line start),
  // 'i' (insert), 'X', RET -> "Xhello".
  expect("hello\x1b"
         "0iX\r",
         "Xhello");

  // vi: "world", ESC, 'x' deletes char under cursor ('d'? cursor on last 'd'),
  // -> "worl", RET.
  expect("world\x1b"
         "x\r",
         "worl");

  // Back to emacs for any later users.
  {
    char line[] = "set editing-mode emacs";
    rl_parse_and_bind(line);
  }
  CHECK(rl_editing_mode == 1);

  if (failures == 0) {
    std::printf("all bind/vi tests passed\n");
    return 0;
  }
  std::fprintf(stderr, "%d bind/vi test(s) failed\n", failures);
  return 1;
}
