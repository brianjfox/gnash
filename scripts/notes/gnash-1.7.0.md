# gnash 1.7.0

A broad bash-conformance release — dozens of `declare`/`unset`/redirection and
expansion behaviors now match bash byte for byte — plus two interactive-polish
fixes for the zsh and csh personalities.

## Interactive

- **zsh menu completion keeps its listing visible.** Under the zsh personality,
  the candidate list shown below the command line on the first TAB now stays put
  while TAB / Shift-TAB cycle through the matches, instead of vanishing the
  moment cycling begins.
- **`personality` works in the csh/tcsh personality.** A session in csh or tcsh
  mode can now run `personality` to query its current mode or switch back out;
  previously it reported `personality: Command not found`.

## Redirections

- **`>&file` redirects both stdout and stderr** to the file (bash's shorthand
  for `&>file`), rather than being mis-read as a descriptor duplication. An
  explicit source fd (`2>&file`) or an input dup (`<&file`) with a non-descriptor
  word is correctly reported as an ambiguous redirect.
- **`noclobber` (`set -o noclobber` / `set -C`)** is implemented: `>` refuses to
  overwrite an existing regular file, while `>|` overrides it.
- A redirection target that expands to zero or several words is now an
  **ambiguous redirect**, matching bash.
- Closing a builtin's output descriptor (`help >&-`) discards its output instead
  of leaking it onto the terminal afterward.
- In POSIX mode a non-interactive shell performs **no pathname expansion** on a
  redirection target (`cat < file.*` opens the literal name).

## `declare` / `typeset` / `local` / `readonly`

- Reject non-identifier operands (`declare /bin/sh`) and `-f`/`-F` with an
  assignment (`cannot use '-f' to make functions`).
- A readonly-assignment failure is reported with the builtin-name prefix
  (`declare: VAR: readonly variable`) and the correct exit status, including when
  the target is reached through a nameref; `+r` can no longer clear an existing
  readonly attribute.
- A declared-but-unassigned variable (`declare -i n`) is treated as unset by
  `${n-word}` and `-v`; the integer attribute is applied before a `declare`
  array assignment; a `local` copy of a readonly global is rejected.
- POSIX `readonly`/`export` listings use the invoking builtin's name.

## Namerefs, arrays, and expansion

- `${a[@]@A}` recreates the whole array as a single `declare` statement
  (matching `declare -p`), instead of emitting a garbled per-element form.
- Array assignment through a nameref that points at an element is rejected as an
  invalid identifier; assigning to an unset nameref drops the attribute with
  bash's `removing nameref attribute` warning; `mapfile`/`read -a` likewise
  reject a nameref that resolves to an array element.
- `unset a[-2]` on an empty or too-short indexed array reports a bad array
  subscript; unquoted `${a[@]OP}` drops elements the operator makes empty.

## Other builtins and options

- `set +o hashall` is honored: `hash` reports `hashing disabled` and `$-` drops
  the `h` flag.
- `set -x` tracing matches bash for `for`/`case` and assignment words; invocation
  option errors and `-O`/`-c` handling match bash.
- `globstar` `**` matches zero path segments and collapses `**/**`.
- `echo` honors the `xpg_echo` shopt and interprets only `\0nnn` octal escapes.
- `bare unalias`/`exec` bad-option print their usage; `jobs` listing matches
  bash's width and `+`/`-` markers; `[[ == ]]` matches quoted glob characters
  literally.
