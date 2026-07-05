// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// shell.hpp -- the interpreter state (variables, options, positional params,
// functions, status) and the top-level run/capture entry points.
#ifndef GNASH_CORE_SHELL_HPP
#define GNASH_CORE_SHELL_HPP

#include <csignal>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "gnash/core/ast.hpp"

namespace gnash::core {

// Set by the interactive SIGINT handler (repl.cpp) when C-c is pressed while a
// command is executing.  The executor's unwinding() check honors it, so the
// running command aborts and the loop reprompts instead of the shell dying.
extern volatile std::sig_atomic_t g_sigint_received;

enum class VarKind { Scalar, Indexed, Assoc };

struct Variable {
  VarKind kind = VarKind::Scalar;
  std::string value;                          // scalar value
  std::map<long long, std::string> idx;        // indexed array
  std::map<std::string, std::string> assoc;    // associative array
  bool exported = false;
  bool readonly = false;
  bool integer = false;
  bool nameref = false;  // `declare -n': value is the name of another variable
};

class Shell {
 public:
  Shell();

  // --- variables ---------------------------------------------------------
  std::map<std::string, Variable> vars;
  bool is_set(const std::string &n) const;
  // Follow a `declare -n' nameref chain to the ultimate target name (with a
  // cycle guard); returns n unchanged when it is not a nameref.
  std::string deref(const std::string &n) const;
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
  // $BASH_ARGC / $BASH_ARGV views (only non-empty inside a function under
  // `shopt -s extdebug'); see bash_argc_view/bash_argv_view in shell.cpp.
  std::vector<std::string> bash_argc_view() const;
  std::vector<std::string> bash_argv_view() const;
  // BASH_ALIASES/BASH_CMDS/BASH_ARGC/BASH_ARGV expose live shell tables as
  // arrays; fills PAIRS (ordered key,value) and returns true for those names.
  bool virtual_array(const std::string &name,
                     std::vector<std::pair<std::string, std::string>> &pairs) const;
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

  // --- shell state for builtins -----------------------------------------
  bool login_shell = false;                  // logout only works in a login shell
  std::map<std::string, std::string> hashed;  // `hash': command name -> full path
  std::map<std::string, bool> shopt_opts;     // `shopt' option states
  std::set<std::string> disabled_builtins;    // `enable -n': builtins turned off
  std::map<std::string, std::string> aliases;  // `alias': name -> expansion
  std::map<std::string, std::string> completions;  // `complete': name -> spec string
  struct CallFrame { int line; std::string func; std::string source; };
  std::vector<CallFrame> call_stack;          // `caller': active function calls

  // --- call/source context for BASH_SOURCE / FUNCNAME / BASH_LINENO ------
  // A frame per active context, bottom = the top-level script.  `name' is the
  // function name, "source" for a sourced file, or "main" for the base script;
  // `source' is the file the frame runs; `line' is the caller line that entered
  // it.  Rebuilt into the three arrays by sync_source_arrays() on push/pop.
  struct SrcFrame { std::string name; std::string source; int line; bool is_func; };
  std::vector<SrcFrame> src_frames;
  std::map<std::string, std::string> func_src;  // function name -> defining file
  std::string current_source() const {          // file of the innermost frame
    return src_frames.empty() ? std::string() : src_frames.back().source;
  }
  void push_src_frame(const std::string &name, const std::string &source, int line, bool is_func);
  void pop_src_frame();
  void sync_source_arrays();  // rewrite BASH_SOURCE/FUNCNAME/BASH_LINENO

  // --- traps -------------------------------------------------------------
  std::map<std::string, std::string> traps;  // signal name (e.g. "EXIT") -> command
  bool in_trap = false;                       // guard against trap recursion
  void set_signal_trap(int signo, bool active);  // (de)install the shared handler
  void run_pending_traps();                   // run traps for signals received
  // Run the DEBUG trap (if set) before a command, with $BASH_COMMAND set to
  // CMD_TEXT.  Only fires inside functions when functrace (-T) is enabled.
  void run_debug_trap(const std::string &cmd_text);

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

  // --- shell personality (which other shell gnash behaves as) -----------
  // gnash can take on the personality of another shell based on its invocation
  // name or the --personality=<name> option.  Persona selects the surface
  // behaviors that differ (startup files, prompt syntax, identity variables,
  // highlighting); behaviors that are a subset of bash's are left unchanged.
  enum class Persona { Bash, Zsh, Ash, Ksh, Csh };
  Persona persona = Persona::Bash;
  std::string personality_name = "gnash";  // exposed as $GNASH_PERSONALITY
  bool is_zsh() const { return persona == Persona::Zsh; }
  bool is_ash() const { return persona == Persona::Ash; }
  bool is_ksh() const { return persona == Persona::Ksh; }
  bool is_csh() const { return persona == Persona::Csh; }
  // csh keeps word-list shell variables in their own namespace (separate from
  // the Bourne `vars'); used only by the csh interpreter (csh.cpp).
  std::map<std::string, std::vector<std::string>> csh_vars;
  bool csh_inited = false;

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
  // Exec-in-place optimization: a forked subshell/command-substitution child
  // whose entire body is a single simple command can `exec' the external
  // directly instead of forking a second time (matching bash).  subshell_leaf
  // marks such a disposable child; can_exec_replace is set once the body is
  // confirmed to be a lone simple command and consumed by the next run_simple.
  bool subshell_leaf = false;
  bool can_exec_replace = false;
  int next_random();             // advance the PRNG, return 0..32767
  bool dynamic_var(const std::string &name, std::string &out);  // RANDOM/SECONDS/...
  static const std::vector<std::string> &special_var_names();   // for completion

  // --- status & options --------------------------------------------------
  int last_status = 0;
  int last_bg_pid = 0;  // $!
  // Exit status of the most recent command substitution, so a pure-assignment
  // command (a=$(cmd)) can take its status, as bash does.
  int last_cmdsub_status = 0;
  bool cmdsub_ran = false;
  void note_cmdsub(int st) { last_cmdsub_status = st; cmdsub_ran = true; }
  bool opt_errexit = false;   // -e
  bool opt_xtrace = false;    // -x
  bool opt_nounset = false;   // -u
  bool opt_noglob = false;    // -f
  bool opt_verbose = false;   // -v
  bool opt_functrace = false; // -T / -o functrace: DEBUG/RETURN traps inherited
  int errexit_suppress = 0;   // >0 while a command's status is being checked
  std::string bash_command;   // $BASH_COMMAND: the command currently executing
  bool in_debug_trap = false; // guard: don't fire the DEBUG trap within itself
  int command_number = 1;     // \# prompt escape: commands entered this session
  bool opt_extdebug = false;  // `shopt -s extdebug': enables BASH_ARGC/BASH_ARGV
  // Call-argument stack for $BASH_ARGC/$BASH_ARGV (only maintained under
  // extdebug).  argframes holds one entry per active function call -- that
  // call's positional arguments ($1..$n, in order) -- pushed on entry, popped
  // on return.  top_positionals snapshots the outermost (script) $1.. so the
  // trailing "main" frame can be reported.  See bash_argc_view/bash_argv_view.
  std::vector<std::vector<std::string>> argframes;
  std::vector<std::string> top_positionals;

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
