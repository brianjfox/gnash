// shell.hpp -- the interpreter state (variables, options, positional params,
// functions, status) and the top-level run/capture entry points.
#ifndef GNASH_CORE_SHELL_HPP
#define GNASH_CORE_SHELL_HPP

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "gnash/core/ast.hpp"

namespace gnash::core {

enum class VarKind { Scalar, Indexed, Assoc };

struct Variable {
  VarKind kind = VarKind::Scalar;
  std::string value;                          // scalar value
  std::map<long long, std::string> idx;        // indexed array
  std::map<std::string, std::string> assoc;    // associative array
  bool exported = false;
  bool readonly = false;
  bool integer = false;
};

class Shell {
 public:
  Shell();

  // --- variables ---------------------------------------------------------
  std::map<std::string, Variable> vars;
  bool is_set(const std::string &n) const;
  std::string get(const std::string &n) const;  // "" if unset
  bool get_if_set(const std::string &n, std::string &out) const;
  void set(const std::string &n, const std::string &v);
  void set_exported(const std::string &n, const std::string &v);
  void export_name(const std::string &n);
  void unset(const std::string &n);

  // --- arrays ------------------------------------------------------------
  std::vector<std::string> array_values(const std::string &n) const;
  std::vector<std::string> array_keys(const std::string &n) const;
  std::string array_get(const std::string &n, const std::string &sub) const;
  void array_set(const std::string &n, const std::string &sub, const std::string &v);
  int array_count(const std::string &n) const;
  // Assign name=(elements...); each element is (optional explicit subscript, value).
  void array_assign(const std::string &n,
                    const std::vector<std::pair<std::optional<std::string>, std::string>> &elems,
                    bool append, bool assoc);
  void make_array(const std::string &n, bool assoc);

  // --- local scopes (for functions / `local' / `declare') ----------------
  void push_scope();
  void pop_scope();
  void make_local(const std::string &n);  // save outer binding, create fresh local
  bool in_function() const { return !local_stack.empty(); }
  std::vector<std::vector<std::pair<std::string, std::optional<Variable>>>> local_stack;

  // --- traps -------------------------------------------------------------
  std::map<std::string, std::string> traps;  // signal name (e.g. "EXIT") -> command
  bool in_trap = false;                       // guard against trap recursion
  void set_signal_trap(int signo, bool active);  // (de)install the shared handler
  void run_pending_traps();                   // run traps for signals received

  // --- job control -------------------------------------------------------
  struct Job {
    int id = 0;                 // %1, %2, ...
    long pgid = 0;              // process-group id
    std::vector<long> pids;     // member pids
    std::string command;        // text for `jobs' listing
    bool running = true;
    bool stopped = false;
    bool done = false;
    int status = 0;             // exit status when done
    bool notified = false;
    bool background = false;
  };
  std::vector<Job> jobs;
  int next_job_id = 1;
  long shell_pgid = 0;
  int job_terminal = -1;        // controlling-terminal fd, or -1
  bool job_control = false;     // interactive + tty
  bool interactive = false;

  void init_job_control(bool interactive_shell);
  Job *add_job(long pgid, const std::vector<long> &pids, const std::string &cmd, bool background);
  Job *job_by_spec(const std::string &spec);  // %n / %% / %+ / %- / pid
  int wait_for_pid(long pid);                  // block, return exit status
  int wait_all();                              // wait for all jobs
  void reap_jobs(bool notify);                 // non-blocking reap (+ optional report)
  bool check_job_events();                     // reap; true if unreported job events exist
  void emit_job_notices();                     // print "[n]+ Done/Stopped"; mark; drop finished
  void print_jobs();
  int foreground_job(Job &j, bool cont);       // bring to foreground, wait
  void background_job(Job &j, bool cont);       // continue in background

  // --- positional parameters --------------------------------------------
  std::vector<std::string> positional;  // $1 == positional[0]
  std::string arg0 = "gnash";

  // --- diagnostics -------------------------------------------------------
  std::string shell_name = "gnash";  // program name shown in error messages
  // "NAME: line N: " prefix that bash prints before runtime errors.
  std::string err_prefix() const {
    return shell_name + ": line " + std::to_string(cur_lineno > 0 ? cur_lineno : 1) + ": ";
  }

  // --- directory stack (pushd/popd/dirs); entries below the current dir ---
  std::vector<std::string> dir_stack;

  // --- process substitutions <(...) / >(...) live for one command ---------
  struct ProcSub { long pid; int fd; };
  std::vector<ProcSub> procsubs;
  // Close fds and wait for substitution children added since index `from`.
  void reap_procsubs(size_t from = 0);

  // --- dynamic special variables ($RANDOM, $SECONDS, $LINENO) ------------
  unsigned long rand_seed = 0;   // RANDOM PRNG state
  bool rand_seeded = false;      // has RANDOM been seeded (explicitly or lazily)
  long long seconds_base = 0;    // epoch second that $SECONDS counts from
  int cur_lineno = 0;            // $LINENO of the command being executed
  int subshell_level = 0;        // $BASH_SUBSHELL: subshell nesting depth
  int next_random();             // advance the PRNG, return 0..32767
  bool dynamic_var(const std::string &name, std::string &out);  // RANDOM/SECONDS/...

  // --- status & options --------------------------------------------------
  int last_status = 0;
  int last_bg_pid = 0;  // $!
  bool opt_errexit = false;   // -e
  bool opt_xtrace = false;    // -x
  bool opt_nounset = false;   // -u
  bool opt_noglob = false;    // -f
  bool opt_verbose = false;   // -v
  int errexit_suppress = 0;   // >0 while a command's status is being checked

  // --- functions ---------------------------------------------------------
  std::map<std::string, const Command *> functions;
  std::vector<CommandPtr> retained;  // keeps eval/source/def trees alive

  // --- control-flow signals (set by builtins, honored by the executor) ---
  int break_count = 0;
  int continue_count = 0;
  bool returning = false;
  bool exiting = false;
  int exit_status = 0;

  // --- helpers -----------------------------------------------------------
  std::string ifs() const;  // IFS value, or default " \t\n"
  std::vector<std::string> environ_block() const;  // NAME=value for exported

  // Parse and execute a script; returns the final exit status.
  int run_string(const std::string &script);
  // Command substitution: run SCRIPT, return its stdout (trailing newlines
  // stripped); *status gets the exit status.
  std::string run_and_capture(const std::string &script, int *status);
  // Function substitution ${ cmd; }: like run_and_capture but in the current
  // shell (no subshell), so side effects such as variable changes persist.
  std::string run_and_capture_inproc(const std::string &script, int *status);
};

// Arithmetic evaluation (arith.cpp): evaluate EXPR in the context of SH.
long long eval_arith(Shell &sh, const std::string &expr, bool *ok);

// Expand a PS1/PS2-style prompt string (prompt.cpp).
std::string expand_prompt(Shell &sh, const std::string &ps);

// The interactive read-eval-print loop (repl.cpp): line editing + history +
// job control.  Returns the shell's final exit status.
int run_interactive(Shell &sh);

}  // namespace gnash::core

#endif  // GNASH_CORE_SHELL_HPP
