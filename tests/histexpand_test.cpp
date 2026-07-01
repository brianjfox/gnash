// histexpand_test.cpp -- unit tests for history_expand() and friends.
//
// History set up as (base 1):
//   1: echo one two three
//   2: ls /usr/local/bin
//   3: cat foo.txt bar.baz

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "readline/history.h"

static int failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                          \
    }                                                                      \
  } while (0)

// Expand SRC and assert the return code and expanded text.
static void expect(const char *src, int want_ret, const char *want_out) {
  char *out = nullptr;
  int r = history_expand(src, &out);
  if (r != want_ret) {
    std::fprintf(stderr, "FAIL expand(\"%s\"): ret %d, wanted %d\n", src, r, want_ret);
    failures++;
  }
  if (out == nullptr || std::strcmp(out, want_out) != 0) {
    std::fprintf(stderr, "FAIL expand(\"%s\"): got \"%s\", wanted \"%s\"\n",
                 src, out ? out : "(null)", want_out);
    failures++;
  }
  std::free(out);
}

// Expand SRC and assert an error (ret -1) whose message contains NEEDLE.
static void expect_error(const char *src, const char *needle) {
  char *out = nullptr;
  int r = history_expand(src, &out);
  CHECK(r == -1);
  if (out == nullptr || std::strstr(out, needle) == nullptr) {
    std::fprintf(stderr, "FAIL expand(\"%s\"): error \"%s\" missing \"%s\"\n",
                 src, out ? out : "(null)", needle);
    failures++;
  }
  std::free(out);
}

static void setup() {
  clear_history();
  add_history("echo one two three");
  add_history("ls /usr/local/bin");
  add_history("cat foo.txt bar.baz");
  using_history();  // interactive pointer to the end, as a real shell does
}

int main() {
  setup();

  // Event designators.
  expect("!!", 1, "cat foo.txt bar.baz");
  expect("!-1", 1, "cat foo.txt bar.baz");
  expect("!1", 1, "echo one two three");
  expect("!echo", 1, "echo one two three");
  expect("!?two?", 1, "echo one two three");

  // Word designators.
  expect("!!:0", 1, "cat");
  expect("!!:2", 1, "bar.baz");
  expect("!!:$", 1, "bar.baz");
  expect("!!:*", 1, "foo.txt bar.baz");
  expect("!ls:$", 1, "/usr/local/bin");

  // Modifiers.
  expect("!ls:$:t", 1, "bin");
  expect("!ls:$:h", 1, "/usr/local");
  expect("!!:$:r", 1, "bar");
  expect("!!:$:e", 1, ".baz");
  expect("!!:$:p", 2, "bar.baz");  // print-only

  // Substitution and quick substitution.
  expect("!!:s/bar/BAR/", 1, "cat foo.txt BAR.baz");
  expect("!!:gs/o/O/", 1, "cat fOO.txt bar.baz");
  expect("^foo^FOO^", 1, "cat FOO.txt bar.baz");

  // Non-expansions.
  expect("echo plain text", 0, "echo plain text");
  expect("no bang here", 0, "no bang here");
  expect("! oops", 0, "! oops");  // '!' + space is inhibited

  // Errors.
  expect_error("!nosuchprefix", "event not found");

  // Tokenizing / argument extraction helpers.
  char **toks = history_tokenize("alpha beta gamma");
  CHECK(toks && std::strcmp(toks[0], "alpha") == 0);
  CHECK(toks[1] && std::strcmp(toks[1], "beta") == 0);
  CHECK(toks[2] && std::strcmp(toks[2], "gamma") == 0);
  CHECK(toks[3] == nullptr);
  for (int i = 0; toks && toks[i]; i++) std::free(toks[i]);
  std::free(toks);

  char *args = history_arg_extract(1, '$', "cmd a b c");
  CHECK(args && std::strcmp(args, "a b c") == 0);
  std::free(args);

  clear_history();

  if (failures == 0) {
    std::printf("all history_expand tests passed\n");
    return 0;
  }
  std::fprintf(stderr, "%d history_expand test(s) failed\n", failures);
  return 1;
}
