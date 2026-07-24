// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// executor.hpp -- walk the AST and run it.
#ifndef GNASH_CORE_EXECUTOR_HPP
#define GNASH_CORE_EXECUTOR_HPP

#include "gnash/core/ast.hpp"
#include "gnash/core/shell.hpp"

namespace gnash::core {

class Executor {
 public:
  explicit Executor(Shell &sh) : sh_(sh) {}

  // Execute a command node; returns its exit status.
  int run(const Command *c);

 private:
  Shell &sh_;
  const Command *timed_cmd_ = nullptr;  // the command currently being `time'd

  int run_simple(const SimpleCommand *c);
  int run_connection(const Connection *c);
  int run_pipeline(const Connection *c);
  int run_subshell(const Subshell *c);
  int run_group(const Group *c);
  int run_if(const IfCommand *c);
  int run_loop(const LoopCommand *c);
  int run_for(const ForCommand *c);
  int run_case(const CaseCommand *c);
  int run_funcdef(const FunctionDef *c);
  int run_cond(const CondCommand *c);
  int run_arith(const ArithCommand *c);
  int run_coproc(const CoprocCommand *c);

  // True if a break/continue/return/exit -- or an interactive C-c -- is
  // unwinding the stack.  Honoring g_sigint_received here makes every run()
  // short-circuit, so a runaway command aborts back to the REPL prompt.
  bool unwinding() const {
    return sh_.break_count || sh_.continue_count || sh_.returning ||
           sh_.exiting || g_sigint_received;
  }
};

}  // namespace gnash::core

#endif  // GNASH_CORE_EXECUTOR_HPP
