// main.cpp -- the gnash executable.
//
//   gnash                 # interactive REPL if stdin is a terminal, else run stdin
//   gnash -c COMMAND [name [args...]]
//   gnash SCRIPT [args...]
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "gnash/core/shell.hpp"

int main(int argc, char **argv) {
  gnash::core::Shell sh;

  std::vector<std::string> args(argv + 1, argv + argc);

  if (!args.empty() && args[0] == "-c") {
    sh.init_job_control(false);
    std::string cmd = args.size() > 1 ? args[1] : "";
    if (args.size() > 2) {
      sh.arg0 = args[2];
      sh.positional.assign(args.begin() + 3, args.end());
    }
    sh.run_string(cmd);
  } else if (!args.empty()) {
    sh.init_job_control(false);
    std::ifstream f(args[0]);
    if (!f) {
      std::fprintf(stderr, "gnash: %s: cannot open\n", args[0].c_str());
      return 127;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    sh.arg0 = args[0];
    sh.positional.assign(args.begin() + 1, args.end());
    sh.run_string(ss.str());
  } else if (isatty(STDIN_FILENO)) {
    return gnash::core::run_interactive(sh);
  } else {
    sh.init_job_control(false);
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    sh.run_string(ss.str());
  }

  int rc = sh.exiting ? sh.exit_status : sh.last_status;

  // Run the EXIT trap, if any, before terminating.
  auto it = sh.traps.find("EXIT");
  if (it != sh.traps.end()) {
    std::string cmd = it->second;
    sh.traps.erase(it);
    sh.exiting = false;
    sh.run_string(cmd);
  }
  return rc;
}
