// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// command_complete_test.cpp -- command-position completion sources
// (keywords, builtins, aliases, and $PATH executables).

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "gnash/core/builtins.hpp"
#include "gnash/core/shell.hpp"

namespace core = gnash::core;

static int failures = 0;

static bool has(const std::vector<std::string> &v, const std::string &s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

// Assert command_completions(prefix) does/does not offer `name'.
static void want(core::Shell &sh, const char *prefix, const char *name, bool expect) {
  std::vector<std::string> v = core::command_completions(sh, prefix);
  if (has(v, name) != expect) {
    std::fprintf(stderr, "FAIL command_completions(\"%s\") %s \"%s\"\n", prefix,
                 expect ? "should contain" : "should not contain", name);
    failures++;
  }
}

int main() {
  core::Shell sh;

  // A temp directory with one executable, used as the entire $PATH.
  char dir[] = "/tmp/gnash_cc_XXXXXX";
  if (!mkdtemp(dir)) {
    std::fprintf(stderr, "mkdtemp failed\n");
    return 1;
  }
  std::string exe = std::string(dir) + "/zqcmd";
  if (FILE *f = std::fopen(exe.c_str(), "w")) { std::fputs("#!/bin/sh\n", f); std::fclose(f); }
  chmod(exe.c_str(), 0755);
  std::string noexe = std::string(dir) + "/zqplain";  // present but not executable
  if (FILE *f = std::fopen(noexe.c_str(), "w")) { std::fputs("x\n", f); std::fclose(f); }
  sh.set("PATH", dir);
  sh.aliases["zqalias"] = "ls";

  want(sh, "ech", "echo", true);       // builtin
  want(sh, "expo", "export", true);    // builtin
  want(sh, "whil", "while", true);     // keyword
  want(sh, "cas", "case", true);       // keyword
  want(sh, "zqal", "zqalias", true);   // alias
  want(sh, "zqc", "zqcmd", true);      // $PATH executable
  want(sh, "zq", "zqalias", true);     // shared prefix -> both alias and PATH exe
  want(sh, "zq", "zqcmd", true);
  want(sh, "zqp", "zqplain", false);   // non-executable file is not a command
  want(sh, "zznomatchzz", "echo", false);  // prefix filters everything out

  // A disabled builtin drops out of the candidates.
  sh.disabled_builtins.insert("echo");
  want(sh, "ech", "echo", false);

  unlink(exe.c_str());
  unlink(noexe.c_str());
  rmdir(dir);

  if (failures == 0) {
    std::printf("all command-completion tests passed\n");
    return 0;
  }
  std::fprintf(stderr, "%d command-completion test(s) failed\n", failures);
  return 1;
}
