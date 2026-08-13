# gnash 2.1.2

A conformance release centered on bash's error semantics: malformed
expansions are rejected the way bash rejects them, fatal errors unwind
the way bash unwinds them, and `errors.tests` now passes byte-for-byte
— 82 of bash 5.3's 83 test suites are byte-perfect.

## Fixes

- An empty array subscript is a bad substitution: `${arr[]}`,
  `${#arr[]}`, `${arr[]:-x}`, `${!arr[]}` and the associative `${A[]}`
  all report `bad substitution` and fail with status 1 instead of
  reading element 0 (#653).
- Trailing text after any parameter reference is a bad substitution —
  `${x!y}`, `${$NO_SUCH_VAR}`, `${-3}`, `${1x}`, `${#x%}` — while the
  undocumented `~` case-invert operator (`${a[0]~}`) and the
  `${!@}`/`${!*}` indirections stay valid (#658).
- A fatal `${...}` expansion error now unwinds the whole command list,
  as bash's DISCARD does: the rest of the input line is abandoned
  through `&&` chains, compounds and function calls; subshells,
  pipelines, command substitutions, `eval` strings and trap bodies
  contain it; a here-document body only aborts the redirection; POSIX
  mode exits 127 (#655).  An invalid `@` transform (`${v@Z}`) is fatal
  like `${x?}`, exit 127.  Loops and `select` report status 1 when
  their condition or word list fails to expand, and a redirection
  target whose expansion fails no longer opens the partially-expanded
  filename.
- `${v:=val}` on a readonly target is an assignment error: reported,
  the list abandoned, fatal in POSIX mode (#658).
- POSIX mode is fatal where bash is fatal: variable-assignment errors
  (standalone, `for`/`select` loop variables, `var=x cmd` prefixes),
  special-builtin redirection failures (even on the left of `||`), and
  export/readonly given an invalid identifier — stopping at the first
  bad name, with `command` shielding all of them (#658).
- The special builtins validate arguments as bash does: `break`/
  `continue` with a non-numeric count are fatal to any non-interactive
  shell; `return` checks its arguments before the can-only-return
  error and returns 2 on a bad status; `exit`/`return`/`shift`/`break`/
  `continue` with extra arguments report `too many arguments`, status
  2, discarding the rest of the list without exiting; `history`
  validates its count argument (#658).
- A failed redirection on a simple command now runs the ERR trap and
  honors `set -e`, with `!` inverting it to success (#656).
- A `{var}<&word` / `{var}>&word` dup redirection requires a plain
  numeric fd: anything else (`foo`, `0junk`, `-1`, an empty expansion)
  is `v: ambiguous redirect`, the variable is left untouched, and an
  out-of-range fd reports `Bad file descriptor` instead of being
  truncated into a valid one (#656).
- `export` validates bare names (`export non-identifier` failed
  silently with status 0), and five `run_builtin` error paths that
  dropped their exit status (`. -x`, `.` without a filename, restricted
  `.`/`exec`/`command -p`) now report it (#658).
- The DEBUG trap fires for `[[ ]]` conditionals like any other leaf
  command (#658).
- Interactive TAB completion offers every builtin the shell actually
  dispatches — including gnash's own `personality`, and `emulate`
  exactly when the zsh personality is active — instead of the
  bash-compatible listing that omits them; `compgen -b` and `enable`
  output are unchanged (#662).

## Build

- Fixed a `-Wdangling-else` error under newer Apple clang.
