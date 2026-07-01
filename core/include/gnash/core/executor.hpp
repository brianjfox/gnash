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

  // True if a break/continue/return/exit is unwinding the stack.
  bool unwinding() const {
    return sh_.break_count || sh_.continue_count || sh_.returning || sh_.exiting;
  }
};

}  // namespace gnash::core

#endif  // GNASH_CORE_EXECUTOR_HPP
