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

// A deliberately out-of-order, mixed-case word list, to check that menu
// completion lists/cycles them in case-insensitive alphabetical order.
static const char *kSortWords[] = {"Delta", "alpha", "Charlie", "bravo", nullptr};

static char *sort_generator(const char *text, int state) {
  static int idx;
  if (state == 0) idx = 0;
  size_t tl = std::strlen(text);
  while (kSortWords[idx]) {
    const char *w = kSortWords[idx++];
    if (std::strncmp(w, text, tl) == 0) return strdup(w);
  }
  return nullptr;
}

static char **sort_attempt(const char *text, int /*start*/, int /*end*/) {
  rl_attempted_completion_over = 1;
  return rl_completion_matches(text, sort_generator);
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

  // zsh-style menu completion: the first TAB only lists the candidates (the
  // line is unchanged); each subsequent TAB inserts the next one, cycling and
  // wrapping.  (foobar/foobaz are the two matches for "foob".)
  rl_bind_key('\t', rl_menu_complete);
  expect("foob\t\n", "foob");          // first TAB lists; line unchanged
  expect("foob\t\t\n", "foobar");      // second TAB inserts the first candidate
  expect("foob\t\t\t\n", "foobaz");    // next candidate
  expect("foob\t\t\t\t\n", "foobar");  // wraps back to the first
  rl_bind_key('\t', rl_complete);      // restore the default TAB completion

  rl_attempted_completion_function = nullptr;

  // Candidates are cycled in case-insensitive alphabetical order regardless of
  // the order the generator produced them (Delta, alpha, Charlie, bravo).
  rl_attempted_completion_function = sort_attempt;
  rl_bind_key('\t', rl_menu_complete);
  expect("\t\t\n", "alpha");          // 1st TAB lists, 2nd inserts sorted[0]
  expect("\t\t\t\n", "bravo");        // sorted[1]
  expect("\t\t\t\t\n", "Charlie");    // sorted[2]
  expect("\t\t\t\t\t\n", "Delta");    // sorted[3]
  rl_bind_key('\t', rl_complete);
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

    // Hidden files: with rl_match_hidden_files off (as in the zsh persona), a
    // leading-dot entry is offered only when the word itself begins with `.'.
    std::string h = std::string(dir) + "/.secret";
    fclose(fopen(h.c_str(), "w"));
    auto has_secret = [](char **mm) {
      bool f = false;
      if (mm) for (int i = 1; mm[i]; i++) if (std::strcmp(mm[i], ".secret") == 0) f = true;
      if (mm) { for (int i = 0; mm[i]; i++) std::free(mm[i]); std::free(mm); }
      return f;
    };
    rl_match_hidden_files = 0;
    CHECK(!has_secret(rl_completion_matches("", rl_filename_completion_function)));
    CHECK(has_secret(rl_completion_matches(".", rl_filename_completion_function)));
    rl_match_hidden_files = 1;  // default (bash) shows them
    CHECK(has_secret(rl_completion_matches("", rl_filename_completion_function)));
    unlink(h.c_str());

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
