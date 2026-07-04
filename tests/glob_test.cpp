// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// glob_test.cpp -- pattern matching (fnmatch/strmatch) and filename globbing.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "gnash/glob.hpp"
#include "glob.h"
#include "strmatch.h"

namespace glob = gnash::glob;

static int failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                          \
    }                                                                      \
  } while (0)

static void m(const char *pat, const char *str, int flags, bool want) {
  bool got = glob::fnmatch(pat, str, flags);
  if (got != want) {
    std::fprintf(stderr, "FAIL fnmatch(\"%s\", \"%s\", %d) = %d, wanted %d\n",
                 pat, str, flags, got, want);
    failures++;
  }
}

static std::string join(const std::vector<std::string> &v) {
  std::string s;
  for (size_t i = 0; i < v.size(); i++) {
    if (i) s += ",";
    s += v[i];
  }
  return s;
}

static void glob_is(const char *pat, int flags, const char *want) {
  std::string got = join(glob::glob(pat, flags));
  if (got != want) {
    std::fprintf(stderr, "FAIL glob(\"%s\") = \"%s\", wanted \"%s\"\n", pat, got.c_str(), want);
    failures++;
  }
}

int main() {
  const int E = FNM_EXTMATCH;

  // -- basic wildcards -----------------------------------------------------
  m("*", "abc", 0, true);
  m("a*c", "abc", 0, true);
  m("a*c", "ac", 0, true);
  m("a?c", "abc", 0, true);
  m("a?c", "ac", 0, false);
  m("*.c", "foo.c", 0, true);
  m("*.c", "foo.h", 0, false);
  m("", "", 0, true);
  m("abc", "abc", 0, true);
  m("abc", "abd", 0, false);

  // -- bracket expressions -------------------------------------------------
  m("[abc]", "b", 0, true);
  m("[abc]", "d", 0, false);
  m("[a-c]", "b", 0, true);
  m("[a-c]", "d", 0, false);
  m("[!a-c]", "d", 0, true);
  m("[!a-c]", "b", 0, false);
  m("[^a-c]", "d", 0, true);
  m("[[:digit:]]", "5", 0, true);
  m("[[:digit:]]", "x", 0, false);
  m("[[:alpha:]]*", "hello", 0, true);
  m("x[[:space:]]y", "x y", 0, true);

  // -- backslash escaping --------------------------------------------------
  m("a\\*c", "a*c", 0, true);
  m("a\\*c", "abc", 0, false);
  m("a\\?c", "a?c", 0, true);

  // -- extended globbing ---------------------------------------------------
  m("@(foo|bar)", "foo", E, true);
  m("@(foo|bar)", "bar", E, true);
  m("@(foo|bar)", "baz", E, false);
  m("+(ab)", "abab", E, true);
  m("+(ab)", "ab", E, true);
  m("+(ab)", "", E, false);
  m("*(ab)", "", E, true);
  m("*(ab)", "ababab", E, true);
  m("?(foo)", "", E, true);
  m("?(foo)", "foo", E, true);
  m("?(foo)", "foofoo", E, false);
  m("!(foo)", "bar", E, true);
  m("!(foo)", "foo", E, false);
  m("foo!(bar)", "foobaz", E, true);
  m("foo!(bar)", "foobar", E, false);
  m("*(a|b)c", "ababc", E, true);

  // -- pathname / period / casefold ----------------------------------------
  // A trailing `*` short-circuits to a match even under FNM_PATHNAME (bash's
  // strmatch behaviour); the slash boundary only applies when `*` is followed
  // by more pattern.  glob() never relies on this -- it splits on `/` itself.
  m("*", "foo/bar", FNM_PATHNAME, true);
  m("*x", "foo/x", FNM_PATHNAME, false);  // `*` cannot cross `/`
  m("*x", "foo/x", 0, true);              // ... but does without FNM_PATHNAME
  m("*/*", "foo/bar", FNM_PATHNAME, true);
  m("*", ".foo", FNM_PERIOD, false);
  m("*", ".foo", 0, true);
  m(".*", ".foo", FNM_PERIOD, true);
  m("ABC", "abc", FNM_CASEFOLD, true);
  m("[a-z]", "B", FNM_CASEFOLD, true);

  // -- filename globbing on a temp tree ------------------------------------
  char tmpl[] = "/tmp/gnash_glob_XXXXXX";
  char *dir = mkdtemp(tmpl);
  CHECK(dir != nullptr);
  if (dir) {
    auto touch = [&](const std::string &rel) {
      std::string p = std::string(dir) + "/" + rel;
      FILE *f = std::fopen(p.c_str(), "w");
      if (f) std::fclose(f);
    };
    std::string subdir = std::string(dir) + "/sub";
    mkdir(subdir.c_str(), 0777);
    touch("a.txt");
    touch("b.txt");
    touch("c.log");
    touch(".hidden");
    touch("sub/x.txt");
    touch("sub/y.md");

    char *cwd = getcwd(nullptr, 0);
    chdir(dir);

    glob_is("*.txt", 0, "a.txt,b.txt");
    glob_is("*", 0, "a.txt,b.txt,c.log,sub");
    glob_is("[ab].txt", 0, "a.txt,b.txt");
    glob_is("c.*", 0, "c.log");
    glob_is("*.md", 0, "");                 // only under sub/
    glob_is("sub/*.txt", 0, "sub/x.txt");
    glob_is("sub/*", 0, "sub/x.txt,sub/y.md");

    // dotfile: ".*" matches the hidden file (and . / ..).
    {
      auto v = glob::glob(".*", 0);
      bool has_hidden =
          std::find(v.begin(), v.end(), ".hidden") != v.end();
      CHECK(has_hidden);
      CHECK(std::find(v.begin(), v.end(), "a.txt") == v.end());
    }

    // globstar
    glob_is("**/*.txt", GX_GLOBSTAR, "a.txt,b.txt,sub/x.txt");

    if (cwd) {
      chdir(cwd);
      std::free(cwd);
    }

    // cleanup
    std::string rm = "rm -rf '" + std::string(dir) + "'";
    if (system(rm.c_str()) != 0) { /* best effort */ }
  }

  if (failures == 0) {
    std::printf("all glob tests passed\n");
    return 0;
  }
  std::fprintf(stderr, "%d glob test(s) failed\n", failures);
  return 1;
}
