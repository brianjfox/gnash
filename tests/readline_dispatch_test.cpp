// readline_dispatch_test.cpp -- drive readline() from a byte stream (no tty)
// and verify the returned line, exercising keymap dispatch end to end.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "readline/history.h"
#include "readline/readline.h"

static int failures = 0;

// Feed INPUT as keystrokes; return the (malloc'd) line readline() produces.
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

static void expect(const std::string &input, const char *want) {
  char *r = run(input);
  if (r == nullptr || std::strcmp(r, want) != 0) {
    std::fprintf(stderr, "FAIL run(%s): got \"%s\", wanted \"%s\"\n",
                 // show input with control chars escaped
                 [&] {
                   static std::string s;
                   s.clear();
                   for (unsigned char c : input) {
                     if (c < 32 || c == 127) {
                       char b[8];
                       std::snprintf(b, sizeof b, "\\x%02x", c);
                       s += b;
                     } else
                       s += static_cast<char>(c);
                   }
                   return s.c_str();
                 }(),
                 r ? r : "(null)", want);
    failures++;
  }
  std::free(r);
}

static void expect_null(const std::string &input) {
  char *r = run(input);
  if (r != nullptr) {
    std::fprintf(stderr, "FAIL: expected NULL (EOF), got \"%s\"\n", r);
    failures++;
  }
  std::free(r);
}

int main() {
  // Plain typing, terminated by newline.
  expect("hello\n", "hello");

  // C-a (beginning) then insert.
  expect("hello\x01X\n", "Xhello");

  // Backspace (DEL) removes the last char.
  expect("abc\x7f\n", "ab");

  // C-a, C-k (kill whole line), C-y (yank back).
  expect("hello world\x01\x0b\x19\n", "hello world");

  // ESC-b (backward-word), ESC-d (kill-word).
  expect("one two\x1b"
         "b\x1b"
         "d\n",
         "one ");

  // Meta-digit numeric argument: ESC 3 x  -> insert 'x' three times.
  expect("\x1b"
         "3x\n",
         "xxx");

  // EOF handling.
  expect_null("");            // empty input -> NULL
  expect_null("\x04");        // C-d on empty line -> NULL
  expect("abc", "abc");       // EOF after text -> return the text

  // History via arrow keys.
  clear_history();
  add_history("first");
  add_history("second");
  expect("\x1b[A\n", "second");            // Up -> most recent
  expect("\x1b[A\x1b[A\n", "first");       // Up Up -> older
  expect("\x1b[A\x1b[B\n", "");            // Up then Down -> back to empty typed line
  clear_history();

  if (failures == 0) {
    std::printf("all readline dispatch tests passed\n");
    return 0;
  }
  std::fprintf(stderr, "%d readline dispatch test(s) failed\n", failures);
  return 1;
}
