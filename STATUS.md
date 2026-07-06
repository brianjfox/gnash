# gnash — Status

Per-module status of the gnash project. This file is the living record of what
each library and the core shell implement; update it as features and modules
land.

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
  - **Incremental search** (`C-r`/`C-s`) and **`.inputrc`** parsing/binding
    (`rl_parse_and_bind`, `rl_bind_keyseq` with `\C-x`/`\M-f`/`\e[A`/`\xNN` syntax,
    `rl_named_function`, `rl_read_init_file`).
  - **vi editing mode** (`set -o vi` / `set editing-mode vi`): motions (`h l 0 ^ $ w W b
    B e E`, `f`/`F`/`t`/`T` with `;`/`,`), the `d`/`c`/`y` operators with counts and the
    doubled `dd`/`cc`/`yy` / `C`/`D`/`S` forms, edits (`x X r ~ s p P`), insert-mode entry
    (`a A i I`), a delete/yank register feeding `p`/`P`, and one-level undo (`u`).
  - **Menu completion** (`rl_menu_complete` / `rl_backward_menu_complete`, used by the zsh
    persona), plus the column-major completion grid.
  - tty raw mode + termcap-based redisplay (single row with horizontal scrolling).

  Try `build/libreadline/gnash_rl_demo`. Remaining refinements: multibyte/UTF-8, yank-pop
  & kill-ring rotation, multi-row wrapped redisplay, and the rest of the `.inputrc`
  directive set (`$if`, macros, all `set` vars).
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
    Verified by a **differential execution harness** (`run_diff.sh`): **161 scripts** produce
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
    `--personality=<name>` (which wins) or its invocation name, exposed as `$GNASH_PERSONALITY`,
    and can switch personality **at runtime** with the `personality` builtin (syntax identical to
    zsh's `emulate`, which is its zsh-mode alias): `personality zsh`, `personality zsh -c '…'`
    (run under a persona then restore), and `personality -L zsh` (local to the enclosing
    function). Personalities:
    - **zsh** — `%`-prompt, `.zsh*` startup, `$ZSH_VERSION`, live command-line syntax
      highlighting, zsh-style tab completion (menu, column-major grid) and `**` globbing,
      global/suffix aliases, and **true-to-zsh parameter expansion**: unquoted `$var` is not
      word-split (while command substitution still is), a bare `$array` expands to all elements,
      1-based array indexing with negatives and `$a[lo,hi]` ranges, brace-free `$a[i]` / `$#a`,
      the `${(flags)…}` expansion flags (join/split/sort/unique/case/keys-values), `${=name}`
      forced splitting, scalar character/substring indexing, and flat key/value associative
      assignment.
    - **ash/dash** — POSIX `$ENV` startup, a `$ ` prompt via parameter expansion.
    - **ksh** — `$KSH_VERSION`, `!`→history-number prompt.
    - **csh/tcsh** — not Bourne-family, so it ships as a self-contained C-shell interpreter
      (its own lexer/parser/evaluator in `core/src/csh.cpp`): `set`/`@`/`setenv`, list variables
      with 1-indexed `$var[n]`/`$#var`/`$?var`, word-initial tilde expansion, backquotes,
      `:h/:t/:r/:e` modifiers, and the `if/foreach/while/switch` control structures.

    Persona fidelity is checked by differential harnesses that run the same scripts under gnash
    and a reference shell, requiring identical stdout + exit status: **csh** vs tcsh 6.21
    (`run_diff_csh.sh`, 36 scripts) and **zsh** vs a reference zsh (`run_diff_zsh.sh`, 43
    scripts), plus a `personality_test.sh` covering the runtime switch.
  - **Interactive REPL** — `gnash` with a terminal starts a full read-eval-print loop that ties
    the whole project together: **libreadline** edits, **libhistory** does history + `!`-
    expansion, the core parses/executes. Prompt expansion (a `\u@\h:\w\$ ` default), multi-line
    continuation, arrow-key recall, `!!`/`^old^new^`, a persistent history file
    (`$HISTFILE`/`~/.gnash_history`), interactive job control, `EXIT`/signal traps, and
    completion — filenames (a trailing `/` for directories), `$`-variable names (incl. the
    dynamic specials), and per-persona syntax highlighting. Verified by driving gnash through a
    pseudo-terminal.
