# gnash

A modular C++ reimplementation of the **GNU Bash** shell, with multiple personalities.

## Rationale
When humans used to write software, the time and effort to create something was much larger than today.  This often created competition between different versions of the same functional software, where people would declare that they liked feature X over feature Y, or that this peice of software was better at doing X.  This kind of religious fervor over which software was *better*, led to operating system distributors having to choose which software would be the default on their systems.

Sometimes, people were confused by licensing, and chose software based on how lenient the licensing was.  **gnash** is an attempt to make all of this nonsense go away, and simply deliver the functionality and features of multiple different shells in the same software package.

I had multiple goals in mind when creating gnash:

1. Create a replacement shell that makes the reasons for having different shells go away. When running scripts, `gnash` should behave *identically* to the personality it is invoked as: **bash**, **ash**, **ksh**, **zsh**, or even **csh**/**tcsh** -- the same stdout/stderr, exit status, side effects, and error semantics.
2. I wanted to connect people to the fact that the relationship of humans to software is changing drastically.  Humans still need to be motivating factor behind the existance of the software, and often, we have architectural goals that are larger than a single, or even a suite, of software.  But writing clean and efficient code is no longer the purview of meat-people.  gnash was conceived of, designed, and written in approximately 3 hours of human attention coupled with 5 hours of computational coding.

## How to Build It
**gnash** needs a C++20 compiler and CMake ≥ 3.16.

- **macOS** — install the Xcode command-line tools and CMake:
  `xcode-select --install`, then `brew install cmake`.
- **Linux** — install a C++20 compiler, CMake, and Make:
  `sudo apt install build-essential cmake` (Debian/Ubuntu) or
  `sudo dnf install gcc-c++ cmake make` (Fedora/RHEL).
- **Windows** — build under **WSL** (Windows Subsystem for Linux) and follow the
  Linux steps; **MSYS2** or **Cygwin** also work. A native MSVC build is not
  supported: gnash relies on POSIX process/terminal APIs (`fork`, `termios`,
  `tcsetpgrp`, `setpgid`, …) that Windows lacks outside those environments.

Then, on any platform:

```bash
cmake -S . -B build -DGNASH_WERROR=ON
cmake --build build -j
```

## How to Run It
**gnash** can be run under many different personalities.  The name of the binary (or the value of the `--personality=XXX` option) control which startup files are read, whether syntax highlighting on the command line exists, and other behaviors.  Currently, **gnash** supports running as:

* **bash** -- reads `~/.bash_profile`, `~/.bashrc`, behaves like **bash-5.3**
* **gnash** -- reads `~/.gnash_profile`, `~/.gnashrc`, behaves like **bash-5.3**
* **zsh** -- reads `~/.zshenv`, `~/.zprofile`, `~/.zshrc`, `~/.zlogin`, uses `%`-style prompts and highlights the command line as you type, behaves like **zsh-5.1**
* **ash** (also **dash**, **sh**) -- reads `/etc/profile`, `~/.profile`, and `$ENV`, uses a plain POSIX `$ ` prompt, behaves like **bash-5.3**
* **ksh** (also **ksh93**, **mksh**, **pdksh**) -- reads `/etc/profile`, `~/.profile`, and `$ENV`, uses a `$ ` prompt in which `!` expands to the history number, behaves like **bash-5.3**
* **csh** (also **tcsh**) -- reads `/etc/csh.cshrc`, `~/.cshrc` (or `~/.tcshrc`), and `~/.login`, and runs the **C shell language** itself: `set`/`@`/`setenv`, `$var[n]` (1-indexed), `$#var`, `$?var`, `` `cmd` ``, `:h`/`:t`/`:r`/`:e` modifiers, `if/then/else/endif`, `foreach`, `while`, and `switch/case/endsw`. Unlike the other personalities (which are surface behavior over the shared Bourne engine), csh is a genuinely different language, so it has its own lexer, parser, and evaluator. Verified against **tcsh 6.21**.

Choose a personality by the binary's name -- e.g. a `zsh` symlink to `gnash`, or a copy named `ksh` -- or with the `--personality=<name>` option, which takes precedence over the name. The active personality is exposed in `$GNASH_PERSONALITY`.

Run it as `build/core/gnash` (interactive), `gnash -c 'CMD'`, or `gnash SCRIPT`.


## Design

gnash is built as a stack of independent libraries, lowest-dependency-first, each a
separate CMake target. Modularity is enforced through target link visibility, so the
dependency graph can't silently erode.

```
libsh        low-level utilities (alloc, quoting, shell-env seam)
  ├─ libhistory     GNU History reimplementation
  ├─ libtilde       tilde expansion
  ├─ libtermcap     terminal capabilities
  ├─ libreadline    line editing + completion hooks
  └─ libglob        pattern matching + filename globbing
core   shell: parse → expand → execute + jobs + REPL    
```

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

- **core** — the shell: a working, interactive, bash-5.3-compatible shell. Landed: the
  **lexer** (quoting, `$(...)`/`` ` ``/`${...}`
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
  - **Expansion + execution** — gnash *runs scripts* and matches bash 5.3. The expander
    implements the `subst.c` pipeline in order: brace, tilde, parameter/`${...}` (defaults,
    length, prefix/suffix removal, substitution, substring, case-mod, and the
    `@Q/@E/@P/@U/@u/@L/@a/@A` transformations — applied element-wise over `${a[@]}`), command
    substitution `$(...)`/`` ` ``, **function substitution `${ cmd; }`**, arithmetic `$(( ))`,
    **process substitution `<(...)`/`>(...)`**, word splitting on `IFS` (quoted-null and
    quoted-glob correct), pathname (via libglob, incl. `nullglob`), and quote removal. Dynamic
    specials: `$RANDOM` (bit-exact to bash's PRNG), `$SECONDS`, `$LINENO`, `$BASHPID`,
    `$BASH_SUBSHELL`, `$EPOCHSECONDS`/`$EPOCHREALTIME`, plus `$BASH_VERSINFO`, `$SHELL`,
    `$MACHTYPE`. The executor runs simple commands, pipelines, `&&`/`||`/`;`/`&`, subshells,
    groups, every compound command, functions (with `local` scoping and a `caller` call stack),
    redirections, and `[[ ]]` (incl. `=~` populating `BASH_REMATCH`) / `(( ))`. Indexed &
    associative arrays, incl. `declare -a a=(...)` and assignment-builtin no-split. `cd`/`pwd`
    track the logical `$PWD` (not the symlink-resolved path). `set -e`/`set -u`/`set -o`,
    `shopt`, and **traps delivered asynchronously between commands** (via a signal-safe pending
    flag). Runtime errors use bash's `name: line N: …` format. **Builtins: the full set bash
    documents** — the core `: echo printf read test cd export unset set eval source …`, plus
    `jobs/fg/bg/disown/kill/suspend`, `pushd/popd/dirs`, `mapfile/readarray`, `alias/unalias`,
    `history/fc`, `hash`, `shopt`, `ulimit`, `enable`, `caller`, `bind`, `compgen/complete/
    compopt`, `help`, and `builtin`; `printf` supports `-v`/`%q`/`%b`, `type` all its flags.
    Verified by a **differential execution harness** (`run_diff.sh`): **152 scripts** produce
    identical stdout and exit status to bash 5.3.
  - **Job control** — each pipeline / background command runs in its own process group; the
    shell hands the controlling terminal to a foreground job and reclaims it, reaps children,
    and maintains a job table. `&` with `$!`, `%n` job specs, and the `jobs/fg/bg/wait/disown/
    kill/suspend` builtins (incl. `128+signum` status). Background-completion notices are
    reported **asynchronously the moment a job finishes**, not just before the next prompt.
  - **Startup, options & personalities** — bash-compliant command-line parsing (grouped short
    flags `-lx`, `+`-unset, long options, `-c`/`-s`/`-l`/`--`), and bash-style startup files
    (`/etc/profile`, `~/.bash_profile` / `~/.bashrc` / `$BASH_ENV`) with the personal-file names
    derived from the invocation. gnash can also take on the **personality** of another shell via
    `--personality=<name>` (which wins) or its invocation name, exposed as `$GNASH_PERSONALITY`:
    **zsh** (`%`-prompt, `.zsh*` startup, `$ZSH_VERSION`, live command-line syntax highlighting),
    **ash/dash** (POSIX `$ENV` startup, a `$ ` prompt via parameter expansion), **ksh**
    (`$KSH_VERSION`, `!`→history-number prompt), and **csh/tcsh** — alongside the default
    bash/gnash. csh is not Bourne-family, so it ships as a self-contained C-shell interpreter
    (its own lexer/parser/evaluator in `core/src/csh.cpp`): `set`/`@`/`setenv`, list variables
    with 1-indexed `$var[n]`/`$#var`/`$?var`, backquotes, `:h/:t/:r/:e` modifiers, and the
    `if/foreach/while/switch` control structures, verified against tcsh 6.21.
  - **Interactive REPL** — `gnash` with a terminal starts a full read-eval-print loop that ties
    the whole project together: **libreadline** edits, **libhistory** does history + `!`-
    expansion, the core parses/executes. Prompt expansion (a `\u@\h:\w\$ ` default), multi-line
    continuation, arrow-key recall, `!!`/`^old^new^`, a persistent history file
    (`$HISTFILE`/`~/.gnash_history`), interactive job control, `EXIT`/signal traps, and
    completion — filenames (a trailing `/` for directories), `$`-variable names (incl. the
    dynamic specials), and per-persona syntax highlighting. Verified by driving gnash through a
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

Four harnesses:

- **Differential** (`tests/harness/run_diff.sh`, gated in ctest) — runs a growing corpus
  of scripts under both gnash and bash 5.3 and requires identical stdout + exit status.
  Currently **152 scripts** covering expansion, arithmetic, control flow, arrays, functions,
  redirection, process substitution, the special variables, the full builtin set, and job
  control — all matching.
- **csh differential** (`tests/harness/run_diff_csh.sh`, gated in ctest when `tcsh` is
  installed) — runs a corpus of C-shell scripts under gnash's csh personality and under a
  reference **tcsh 6.21**, requiring identical stdout + exit status. Currently **36 scripts**
  covering `set`/`@` arithmetic, list variables and indexing, `:` modifiers, backquotes,
  `if`/`foreach`/`while`/`switch`, pipelines and redirection — all matching.
- **Error format** (`tests/harness/errfmt.sh`, gated in ctest) — diffs stderr for a set of
  error cases against bash, normalizing the leading program name. gnash now emits bash's
  exact `name: line N: context: message` format; the only difference is the program name
  (`gnash` vs `bash`), which is inherent.
- **bash test suite** (`tests/harness/conformance.sh`) — runs bash's own `tests/*.tests`
  under gnash (with `$THIS_SH`/env set up like bash's `run-all`, and the `recho`/`zecho`
  helpers built) and diffs against the checked-in `.right` files. This is a *progress
  metric*, not a hard gate: many `.right` files capture the exact program name in error text
  and self-test summaries, so a behaviourally-correct gnash still diverges on those lines.
  The differential and error-format harnesses (individual constructs cross-checked against
  real bash) are the more representative signal. The tests gnash reproduces exactly are
  pinned as a ctest regression gate (`conformance_gate.sh`).
