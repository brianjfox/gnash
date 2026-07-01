# gnash

A modular C++ reimplementation of **bash 5.3**.

The overriding goal: when running scripts, `gnash` behaves **identically** to bash 5.3
— same stdout/stderr, exit status, side effects, and error semantics. Job-control
fidelity is a first-class concern. Structural reference is the bash 5.3 source in
`../bash`; the definitive behavioral manual is `../bash/doc/bash.pdf`.

## Design

gnash is built as a stack of independent libraries, lowest-dependency-first, each a
separate CMake target. Modularity is enforced through target link visibility, so the
dependency graph can't silently erode.

```
libsh        low-level utilities (alloc, quoting, shell-env seam)
  ├─ libhistory     GNU History reimplementation        [complete]
  ├─ libtilde       tilde expansion                      [complete]
  ├─ libtermcap     terminal capabilities                [complete]
  ├─ libreadline    line editing + completion hooks      [functional]
  └─ libglob        pattern matching + filename globbing  [complete]
core   shell: parse → expand → execute + jobs + REPL        [interactive]
       (next: conformance vs bash tests/, funsub, more builtins)
```

Run it: `build/core/gnash` (interactive), `gnash -c 'CMD'`, or `gnash SCRIPT`.

Each library exposes a **modern C++ API** under `namespace gnash::*` (headers in
`include/gnash/`) plus a **thin C shim** with the classic names and ABI (headers in
`compat/`), so existing programs can link a drop-in replacement.

## Status

- **libsh** — checked allocation helpers (`gnash::sh`).
- **libtilde** — complete: `tilde_expand`/`tilde_expand_word`/`tilde_find_word` and the
  preexpansion/failure hooks and additional prefixes/suffixes, ported from `tilde.c`.
  C++ wrapper `<gnash/tilde.hpp>`, C shim `<readline/tilde.h>`. The `sh_get_env_value`
  seam (libsh) lets the shell later resolve `~` against shell variables.
- **libtermcap** — complete: the capability engine (`tgetent`/`tgetstr`/`tgetnum`/
  `tgetflag`) and output/parameter routines (`tputs`/`tparam`/`tgoto`) ported from
  bash's `termcap.c`/`tparam.c`, plus a compiled-in database (xterm/vt100/ansi/dumb) so
  it works with no `/etc/termcap` (macOS). C++ wrapper `<gnash/termcap.hpp>`, C shim
  `<termcap.h>`. `tgoto` cursor addressing is verified.
- **libreadline** — functional. `readline()` is interactive and covers the major
  subsystems:
  - Editing core: line buffer, `rl_point`/`rl_end`/`rl_mark`, kill ring (emacs accumulation),
    and the bindable `int(int,int)` commands.
  - Keymaps + dispatch: programmatic **emacs** (standard/meta/ctlx + CSI/SS3) and **vi**
    (insertion/movement) keymaps; ESC/meta and Ctrl-X prefixes; numeric arguments
    (`M-<digit>`, `C-u`); history movement (`C-p`/`C-n`/arrows).
  - **Completion + hooks** (the design centerpiece): `rl_complete`/`rl_possible_completions`,
    `rl_completion_matches`, filename & username generators, and the hook seam
    (`rl_attempted_completion_function` / `rl_completion_entry_function` / `rl_completer_*`)
    the shell plugs programmable completion into. Bound to TAB / `M-?`.
  - **Incremental search** (`C-r`/`C-s`), **vi mode** (`set editing-mode vi`), and
    **`.inputrc`** parsing/binding (`rl_parse_and_bind`, `rl_bind_keyseq` with
    `\C-x`/`\M-f`/`\e[A`/`\xNN` syntax, `rl_named_function`, `rl_read_init_file`).
  - tty raw mode + termcap-based redisplay (single row with horizontal scrolling).

  Try `build/libreadline/gnash_rl_demo`. Remaining refinements: multibyte/UTF-8, full vi
  operators/counts/undo, yank-pop & kill-ring rotation, menu-complete, multi-row wrapped
  redisplay, and the rest of the `.inputrc` directive set (`$if`, macros, all `set` vars).
- **libhistory** — complete: history list management, navigation, search, state, file
  I/O, and full history expansion (`history_expand`, `history_arg_extract`,
  `history_tokenize`, `get_history_event`) — faithful to `history.c` / `histfile.c` /
  `histsearch.c` / `histexpand.c`. Modern API in `<gnash/history.hpp>`; drop-in C shim in
  `<readline/history.h>`. Expansion output is verified against bash 5.3 for the event
  designators, word specifiers, and modifiers (`:h :t :r :e :p :s :g :q :x`).
- **libglob** — complete: `strmatch`/`fnmatch` (a byte-oriented port of `sm_loop.c` — `*`,
  `?`, `[...]` with POSIX `[:class:]`/`[.sym.]`/`[=eq=]`, and the ksh extended operators
  `?(..) *(..) +(..) @(..) !(..)`) plus `glob_filename` (recursive per-component expansion,
  dotfile rules, `**` globstar, sorted/deduped). C++ API `<gnash/glob.hpp>`; C shims
  `<strmatch.h>` and `<glob.h>`. Extended-glob and bracket results verified against bash 5.3.
  Locale collation is simplified to C/ASCII; wide-character matching is a later addition.

- **core** — the shell, in progress. Landed: the **lexer** (quoting, `$(...)`/`` ` ``/`${...}`
  opaque spans, extended-glob patterns, array-assignment words, operators, IO numbers,
  comments, line continuation, here-document collection, and unterminated-span detection) and
  a recursive-descent **parser → AST** (`gnash::core`) covering lists, and-or, pipelines
  (incl. `|&`, and interleaved `!`/`time`, bare `!`), simple commands with redirections (incl.
  here-docs/here-strings), subshells, groups, `if`/`while`/`until`/`for`/`case`, function
  definitions, **`[[ ]]` conditionals, `(( ))` arithmetic commands, arithmetic-`for`, `select`,
  and `coproc`** (with `(( … ))`-vs-`( ( … ) )` disambiguation via backtracking). The AST
  mirrors bash's `command.h`; `to_string` gives a re-parseable canonical form. `gnash-parse`
  syntax-checks/dumps a script with the same accept/reject contract as `bash -n`. Verified by
  round-trip tests and a **differential harness against `bash -n`**; parses all 83 bash test
  files with no crashes and **74/83 accept/reject agreement** — the remaining few are `bash -n`
  oracle limits (extglob/printf checked without `shopt`) or deep edge cases (comments inside
  `$(...)`, array-element syntax, exported-function env encoding).
  - **Expansion + execution** — gnash now *runs scripts*. The expander implements the
    `subst.c` pipeline in order: brace expansion, tilde, parameter/`${...}` (defaults, length,
    prefix/suffix removal, substitution, substring, case-mod), command substitution
    `$(...)`/`` ` ``, arithmetic `$(( ))`, word splitting on `IFS`, pathname (via libglob), and
    quote removal. A recursive-descent arithmetic evaluator backs `$(( ))`/`(( ))`. The executor
    runs simple commands (PATH exec + fork), pipelines, `&&`/`||`/`;`/`&`, subshells, groups,
    `if`/`while`/`until`/`for` (incl. arithmetic-for)/`case`, functions (with `local` scoping),
    redirections (incl. here-docs/here-strings/dup/`&>`), and `[[ ]]`/`(( ))`. **Indexed and
    associative arrays** (`a=(...)`, `a[i]=`, `a+=(...)`, `${a[@]}`, `${#a[@]}`, `${!a[@]}`,
    `declare -A`). Options: `set -e` (errexit, with condition-context suppression) and `set -u`
    (nounset). **Traps** (`trap … EXIT` and by-signal storage). Builtins: `: true false echo
    printf cd pwd export unset set shift exit return break continue eval source/. read test/[
    local declare typeset readonly let type trap umask getopts exec command times wait`. The
    `gnash` binary runs `gnash -c CMD`, `gnash SCRIPT`, or stdin. Verified by a **differential
    execution harness** (`run_diff.sh`): 63 scripts spanning all of the above produce identical
    stdout and exit status to bash 5.3.
  - **Job control** — each pipeline / background command runs in its own process group;
    the shell hands the controlling terminal to a foreground job and reclaims it (when
    interactive), reaps children, and maintains a job table. `&` backgrounding with `$!`,
    and the builtins `jobs`/`fg`/`bg`/`wait`/`disown`/`kill` (with signal-name lookup). The
    63→70-script differential harness now also covers backgrounding, `wait`/`$!`, `kill`,
    and multi-stage pipelines feeding builtins — all matching bash 5.3 (incl. `128+signum`
    exit status and correct output ordering across the stdio/redirection boundary).
    Not yet: array literals *via* `declare -a name=(...)` (prefix `name=(...)` works),
    `Ctrl-Z`/`fg` round-trip, signal-trap delivery, full option set.
  - **Interactive REPL** — `gnash` with a terminal starts a full read-eval-print loop that
    ties the whole project together: **libreadline** does the line editing, **libhistory**
    does history + `!`-expansion, and the shell core parses/executes. Prompt expansion
    (`PS1`/`PS2` with `\u \h \w \W \$ \s \v \t \d …`), multi-line continuation for compound
    commands, unterminated quotes, here-documents and trailing `\`, arrow-key history recall,
    `!!`/`^old^new^` expansion, a persistent history file (`$HISTFILE`/`~/.gnash_history`),
    interactive job control, and the `EXIT` trap. Verified by driving gnash through a
    pseudo-terminal.

## Build & test

`cmake` is not always on `PATH`; any CMake ≥ 3.16 works.

```sh
cmake -S . -B build -G "Unix Makefiles" -DGNASH_WERROR=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Options: `-DGNASH_SANITIZE=ON` (ASan/UBSan), `-DGNASH_BUILD_TESTS=OFF`.

## Conformance

Two harnesses:

- **Differential** (`tests/harness/run_diff.sh`, gated in ctest) — runs a growing corpus
  of scripts under both gnash and bash 5.3 and requires identical stdout + exit status.
  Currently 70 scripts covering expansion, arithmetic, control flow, arrays, functions,
  redirection, and job control — all matching.
- **bash test suite** (`tests/harness/conformance.sh`) — runs bash's own `tests/*.tests`
  under gnash (with `$THIS_SH`/env set up like bash's `run-all`, and the `recho`/`zecho`
  helpers built) and diffs against the checked-in `.right` files. This is a *progress
  metric*, not a hard gate: many `.right` files capture bash's exact error-message text
  (`bash: line N: …: command not found`) and self-test summaries, so a behaviourally-correct
  gnash still diverges on those lines. Current baseline: **2/82 byte-identical** — the
  differential harness (individual constructs cross-checked against real bash) is the more
  representative signal. The tests gnash reproduces exactly are pinned as a ctest regression
  gate (`conformance_gate.sh`).
