// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// readline_complete_test.cpp -- completion engine + hooks, and isearch.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#include "readline/history.h"
#include "readline/readline.h"

static int failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                          \
    }                                                                      \
  } while (0)

// ---- a fixed word list used by the custom completion hook ----------------
static const char *kWords[] = {"foobar", "foobaz", "fizz", nullptr};

static char *word_generator(const char *text, int state) {
  static int idx;
  if (state == 0) idx = 0;
  size_t tl = std::strlen(text);
  while (kWords[idx]) {
    const char *w = kWords[idx++];
    if (std::strncmp(w, text, tl) == 0) return strdup(w);
  }
  return nullptr;
}

static char **attempt(const char *text, int /*start*/, int /*end*/) {
  rl_attempted_completion_over = 1;  // don't fall back to filenames
  return rl_completion_matches(text, word_generator);
}

// Feed INPUT to readline(); return the produced line.
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
  // -- rl_completion_matches: longest common prefix + entries --------------
  char **m = rl_completion_matches("foo", word_generator);
  CHECK(m != nullptr);
  if (m) {
    CHECK(std::strcmp(m[0], "fooba") == 0);   // lcd of foobar/foobaz
    int n = 0;
    for (int i = 1; m[i]; i++) n++;
    CHECK(n == 2);
    for (int i = 0; m[i]; i++) std::free(m[i]);
    std::free(m);
  }

  // -- completion through the attempted-completion hook --------------------
  rl_attempted_completion_function = attempt;

  // Ambiguous: "fo" -> common prefix "fooba" (no append, no accept yet).
  expect("fo\t\n", "fooba");
  // Unique: "fi" -> "fizz" + append space.
  expect("fi\t\n", "fizz ");

  rl_attempted_completion_function = nullptr;

  // -- filename completion in a temp directory -----------------------------
  char tmpl[] = "/tmp/gnash_comp_XXXXXX";
  char *dir = mkdtemp(tmpl);
  CHECK(dir != nullptr);
  if (dir) {
    std::string a = std::string(dir) + "/alpha";
    std::string b = std::string(dir) + "/alto";
    std::string c = std::string(dir) + "/beta";
    fclose(fopen(a.c_str(), "w"));
    fclose(fopen(b.c_str(), "w"));
    fclose(fopen(c.c_str(), "w"));

    char *cwd = getcwd(nullptr, 0);
    chdir(dir);
    char **fm = rl_completion_matches("al", rl_filename_completion_function);
    CHECK(fm != nullptr);
    if (fm) {
      CHECK(std::strcmp(fm[0], "al") == 0);  // lcd of alpha/alto
      int n = 0;
      for (int i = 1; fm[i]; i++) n++;
      CHECK(n == 2);
      for (int i = 0; fm[i]; i++) std::free(fm[i]);
      std::free(fm);
    }
    char **fm2 = rl_completion_matches("alp", rl_filename_completion_function);
    CHECK(fm2 && fm2[1] && fm2[2] == nullptr);  // unique -> alpha
    if (fm2) {
      CHECK(std::strcmp(fm2[1], "alpha") == 0);
      for (int i = 0; fm2[i]; i++) std::free(fm2[i]);
      std::free(fm2);
    }
    if (cwd) {
      chdir(cwd);
      std::free(cwd);
    }
    unlink(a.c_str());
    unlink(b.c_str());
    unlink(c.c_str());
    rmdir(dir);
  }

  // -- incremental reverse search ------------------------------------------
  clear_history();
  add_history("foobar");
  add_history("foobaz");
  add_history("find me");
  // C-r, type "bar", RET -> matches "foobar".
  expect("\x12"
         "bar\r",
         "foobar");
  // C-r, type "baz", RET -> matches "foobaz".
  expect("\x12"
         "baz\r",
         "foobaz");
  clear_history();

  if (failures == 0) {
    std::printf("all completion/isearch tests passed\n");
    return 0;
  }
  std::fprintf(stderr, "%d completion/isearch test(s) failed\n", failures);
  return 1;
}
