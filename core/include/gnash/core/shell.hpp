// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// shell.hpp -- the interpreter state (variables, options, positional params,
// functions, status) and the top-level run/capture entry points.
#ifndef GNASH_CORE_SHELL_HPP
#define GNASH_CORE_SHELL_HPP

#include <csignal>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
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
  std::vector<std::string> assoc_seq;          // assoc key insertion order (for
                                               // bash-compatible hash iteration)
  bool exported = false;
  bool readonly = false;
  bool integer = false;
  bool nameref = false;  // `declare -n': value is the name of another variable
  bool ucase = false;    // `declare -u': uppercase the value on every assignment
  bool lcase = false;    // `declare -l': lowercase the value on every assignment
  bool capcase = false;  // `declare -c': capitalize the value on every assignment
  bool invisible = false;  // declared with no value (`declare -a b'): unset, so
                           // `declare -p' prints it without a `=' / `=()' value
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
  // If following N's nameref chain lands on a subscripted target `base[sub]'
  // (`declare -n ref=arr[2]'), split it into BASE and SUB (raw subscript text,
  // evaluated by array_get/array_set) and return true; otherwise false.
  bool nameref_elt(const std::string &n, std::string &base, std::string &sub) const;
  std::string get(const std::string &n) const;  // "" if unset
  bool get_if_set(const std::string &n, std::string &out) const;
  // Assign N=V; false if N is readonly (an error is printed).
  bool set(const std::string &n, const std::string &v);
  void set_exported(const std::string &n, const std::string &v);
  void export_name(const std::string &n);
  // Remove N.  A readonly variable is left in place unless FORCE is set
  // (bash's unbind_variable_noref, used by getopts to clear OPTARG).  When
  // NOREF is set (`unset -n'), the named nameref variable itself is removed
  // rather than the variable it points at.
  void unset(const std::string &n, bool force = false, bool noref = false);

  // --- arrays ------------------------------------------------------------
  std::vector<std::string> array_values(const std::string &n) const;
  std::vector<std::string> array_keys(const std::string &n) const;
  // Associative-array keys in bash's hash-table iteration order (bucket 0..N,
  // and within a bucket newest-first), so `${a[@]}' etc. match bash byte for
  // byte.  Public so `declare -p' can print in the same order.
  static std::vector<std::string> assoc_order(const Variable &v);
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
  // True when NAME holds an indexed or associative array (not a scalar); used
  // by the zsh personality, where a bare `$array' expands to all its elements.
  bool is_array(const std::string &n) const;
  // zsh uses 1-based array subscripts (and negative indices counting from the
  // end).  Under the zsh personality, translate such a subscript on an indexed
  // array to gnash's internal 0-based index; for scalars, associative arrays,
  // or any other personality the subscript is returned unchanged.
  std::string zsh_subscript(const std::string &name, const std::string &sub) const;
  // Assign name=(elements...); each element is (optional explicit subscript, value).
  void array_assign(const std::string &n,
                    const std::vector<std::pair<std::optional<std::string>, std::string>> &elems,
                    bool append, bool assoc);
  void make_array(const std::string &n, bool assoc);

  // --- local scopes (for functions / `local' / `declare') ----------------
  void push_scope();
  void pop_scope();

  // getopts character-scan state (bash's sh_charindex/nextchar), kept here so
  // it is saved and restored around a function's `local OPTIND', matching
  // bash's per-scope getopt state.
  size_t getopt_charidx = 1;
  std::string getopt_curarg;
  int getopt_optind = 0;
  void make_local(const std::string &n);  // save outer binding, create fresh local
  bool in_function() const { return !local_stack.empty(); }
  std::vector<std::vector<std::pair<std::string, std::optional<Variable>>>> local_stack;
  // Per-scope saved getopt state, set when that scope localizes OPTIND.
  std::vector<std::optional<std::tuple<size_t, std::string, int>>> getopt_scope_saves;

  // --- shell state for builtins -----------------------------------------
  bool login_shell = false;                  // logout only works in a login shell
  std::map<std::string, std::string> hashed;  // `hash': command name -> full path
  std::map<std::string, bool> shopt_opts;     // `shopt' option states
  std::set<std::string> disabled_builtins;    // `enable -n': builtins turned off
  std::map<std::string, std::string> aliases;  // `alias': name -> expansion
  std::map<std::string, std::string> global_aliases;  // zsh `alias -g': expand anywhere
  std::map<std::string, std::string> suffix_aliases;  // zsh `alias -s ext=cmd'
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
  // Function name -> the lineno_base in effect where it was defined, so $LINENO
  // inside the body reports absolute source lines even when the function is
  // called from a different input block (which has its own lineno_base).
  std::map<std::string, int> func_lineno_base;
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
  void note_child_reaped();                   // count a reaped child for the SIGCHLD trap
  int pending_sigchld = 0;                     // children reaped, awaiting the CHLD trap
  // Run the DEBUG trap (if set) before a command, with $BASH_COMMAND set to
  // CMD_TEXT.  Only fires inside functions when functrace (-T) is enabled.
  // Run the DEBUG trap; returns its exit status (extdebug: non-zero skips
  // the command about to run).
  int run_debug_trap(const std::string &cmd_text);
  void run_err_trap(int status);              // run the ERR trap after a failure
  int run_return_trap(int status);            // run the RETURN trap on func/source return

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
  char invocation_char = 0;     // $- invocation letter: 'c' (-c), 's' (stdin), else 0

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
  // Select the active personality by name (zsh/sh/dash/ash/ksh.../csh/tcsh/bash,
  // same mapping as at startup): sets `persona', $GNASH_PERSONALITY, and the
  // per-shell identity variables ($ZSH_VERSION / $KSH_VERSION / $BASH_VERSION,
  // ...).  Used both at startup and by the `personality'/`emulate' builtin to
  // switch personality while the shell is running.
  void set_personality(const std::string &name);
  // Invoked at the end of set_personality (once set), so an interactive REPL can
  // re-apply persona-dependent readline hooks (highlighting, TAB completion
  // style, ...) when the personality changes at runtime.  Null until registered.
  std::function<void()> on_personality_change;
  // Per-function-call saved personality for `personality -L' / `emulate -L':
  // each function call pushes an empty slot; a -L switch records the personality
  // to restore into the current slot; the slot is restored on function return.
  std::vector<std::optional<std::string>> persona_restore;
  // csh keeps word-list shell variables in their own namespace (separate from
  // the Bourne `vars'); used only by the csh interpreter (csh.cpp).
  std::map<std::string, std::vector<std::string>> csh_vars;
  bool csh_inited = false;

  // --- diagnostics -------------------------------------------------------
  std::string shell_name = "gnash";  // program name shown in error messages
  // Extra context component for parse errors ("eval", "command substitution"),
  // as bash prints `NAME: eval: line N: ...'.
  std::string error_context;
  // "NAME: line N: " prefix that bash prints before runtime errors.
  std::string err_prefix() const {
    return shell_name + ": line " + std::to_string(cur_lineno > 0 ? cur_lineno : 1) + ": ";
  }

  // --- directory stack (pushd/popd/dirs); entries below the current dir ---
  std::vector<std::string> dir_stack;
  // The full stack as $DIRSTACK / ~N sees it: logical $PWD at [0], then
  // dir_stack.  Defined in builtins.cpp (uses logical_pwd).
  std::vector<std::string> dirstack() const;

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
  bool opt_keyword = false;   // -k: assignment-form words anywhere are assignments
  bool opt_physical = false;  // -P: resolve symlinks in cd/pwd; shown in $-/SHELLOPTS
  bool opt_xtrace = false;    // -x
  bool opt_nounset = false;   // -u
  bool opt_noglob = false;    // -f
  bool opt_verbose = false;   // -v
  bool opt_noexec = false;    // -n: read/parse commands but don't execute them
  bool opt_pipefail = false;  // -o pipefail: pipeline status = last non-zero stage
  bool opt_functrace = false; // -T / -o functrace: DEBUG/RETURN traps inherited
  bool opt_history = false;     // -o history: record command lines in the history
  bool opt_histexpand = false;  // -H / -o histexpand: `!' history expansion
  bool history_loaded = false;  // $HISTFILE has been read into the history list
  int hist_new_entries = 0;     // entries added this session (for `history -a')
  // History index of the currently-executing command's own entry (so `fc'
  // excludes and later replaces it), or -1 when it was not recorded.
  int hist_cur_cmd_index = -1;
  int lineno_base = 0;          // added to AST line numbers ($LINENO, errors) when
                                // a script runs command-by-command

  bool opt_posix = false;       // -o posix
  bool opt_restricted = false;  // -r / -o restricted / rbash: restricted shell
  // Set when a top-level $((...)) / $[...] arithmetic expansion failed (bad
  // expression or an assignment to a readonly variable); run_simple aborts the
  // current command (bash aborts but the shell continues).
  bool arith_error = false;
  // Turn on `-o history': the first enable loads $HISTFILE and applies the
  // $HISTSIZE stifle, as bash does.
  void enable_history();
  // Point the history library's expansion characters at $histchars.
  void sync_histchars();
  // Add LINE to the history honoring $HISTCONTROL and $HISTIGNORE; true if
  // it was saved.
  bool add_history_line(const std::string &line);
  // Append a continuation LINE of a multi-line command to the newest history
  // entry (shopt cmdhist), joined with bash's delimiting rules (or a newline
  // inside a here-document).
  void append_history_line(const std::string &line, bool heredoc = false);
  int errexit_suppress = 0;   // >0 while a command's status is being checked
  std::string bash_command;   // $BASH_COMMAND: the command currently executing
  int trap_sig = 0;           // $BASH_TRAPSIG: signal number while a trap runs
  bool in_debug_trap = false; // guard: don't fire the DEBUG trap within itself
  bool in_err_trap = false;   // guard: don't fire the ERR trap within itself
  bool in_return_trap = false;// guard: don't fire the RETURN trap within itself
  // The DEBUG trap body as it stood when each active function was entered.  A
  // DEBUG trap the function installs for itself (the body differs from entry)
  // fires without functrace; an inherited one does not.
  std::vector<std::string> debug_frame;
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
  std::set<std::string> exported_functions;  // `export -f': passed to children
  // Import `BASH_FUNC_name%%=() {...}' definitions from the environment (called
  // once at startup); parses each safely, ignoring any trailing commands.
  void import_env_functions();

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
  // Run a script buffer command-by-command (as bash reads scripts), so history
  // recording/expansion and mid-script alias definitions behave like bash.
  int run_script_lines(const std::string &text);
  // Command substitution: run SCRIPT, return its stdout (trailing newlines
  // stripped); *status gets the exit status.
  std::string run_and_capture(const std::string &script, int *status);
  // Function substitution ${ cmd; }: like run_and_capture but in the current
  // shell (no subshell), so side effects such as variable changes persist.
  std::string run_and_capture_inproc(const std::string &script, int *status);
};

// Arithmetic evaluation (arith.cpp): evaluate EXPR in the context of SH.
long long eval_arith(Shell &sh, const std::string &expr, bool *ok);
// eval_arith plus bash's error diagnostics; cmd_name selects the prefix:
// "" for $((...)), "((" for the (( )) command, "let" for the let builtin.
long long eval_arith_msg(Shell &sh, const std::string &expr, const char *cmd_name, bool *ok);

// Expand a PS1/PS2-style prompt string (prompt.cpp).
std::string expand_prompt(Shell &sh, const std::string &ps);

// The interactive read-eval-print loop (repl.cpp): line editing + history +
// job control.  Returns the shell's final exit status.
int run_interactive(Shell &sh);

}  // namespace gnash::core

#endif  // GNASH_CORE_SHELL_HPP
