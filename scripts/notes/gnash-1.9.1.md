# gnash 1.9.1

Two more of bash's test files now pass byte-for-byte -- `test` and `exp`
-- bringing the total to **60 of 83**.  Namerefs, the `test` builtin and
`[[ =~ ]]` regular expressions all move a long way closer to bash, and
three bugs reported from outside are fixed.

## The `test` builtin

`test` / `[` is now a faithful port of bash's `test.c`.  It dispatches on
the argument count, so `test -a noexist` is the `-a` primary while
`test x -a y` is the `and` connective -- a distinction that cannot be
made by looking at the tokens alone.  Anything outside the recognized
operator sets is a diagnostic with status 2 rather than a silent
fallback, so `test -A x` and `test a b c d e` no longer answer true.

- The missing primaries `-a -g -k -u -G -O -N -S -o -R` are implemented,
  along with the file comparisons `-nt -ot -ef`.  A file that cannot be
  stat'd is older than one that can, so `test noexist -ot existing` is
  true.
- Operands of `-eq` and friends must be integers: `test 4+3 -eq 7` is
  `4+3: integer expected`, not an arithmetic evaluation.
- `-t` takes an optional argument outside posix mode, so `test -t` and
  `test -t -a -t` both mean `-t 1`.
- `$GROUPS` is populated, with the real gid first.

## Namerefs

- Temporary assignments bind the reference's target, so `ref=xxx cmd`
  puts `var=xxx` in the environment and leaves `ref` alone; `export ref`
  likewise exports the target, and `export -n` now works at all.
- `declare`/`local` on a nameref localizes the target rather than the
  reference -- but only when the nameref lives at the same scope, so a
  `local ref` that shadows an outer nameref still localizes `ref`.
- A nameref aimed at an array element resolves for `unset` as well, and
  an assignment whose chain loops back to an element of the reference
  drops the nameref attribute and makes it an array.
- `declare -i -n NAME=VALUE` fails, as it must: the integer attribute
  makes the assignment arithmetic, and a number is not a valid target.
  Adding `-n` no longer strips attributes given on the same command.
- `declare -n` on a variable holding an empty string is rejected.

## Conditional expressions

A quoted part of a `[[ ... =~ ... ]]` operand now matches literally, so
`[[ a =~ "[[:alpha:]]" ]]` looks for that eleven-character string.  Only
the quoted characters that are special in a POSIX ERE are escaped, and
inside a bracket expression nothing is, so `[[ "\" =~ ["."] ]]` does not
match a backslash.  A pattern that will not compile reports
`invalid regular expression` with status 2 instead of quietly answering
"no match".

## Assignment errors

A failed assignment in a command with no command word abandons the rest
of the command list, as bash does: `RO=z ; echo hi` prints nothing.  The
unwind escapes functions and loops while a subshell contains it, and
assignments before the failing one still take effect.

## Coprocesses and mapfile

- A coprocess's `NAME` and `NAME_PID` are unset once it has been reaped,
  so they stop advertising file descriptors whose process is gone.
- `mapfile` creates its array before filling it, so the variable exists
  even when the input is empty and keeps its attributes; an existing
  associative array is refused.
- `mapfile` and `read -a` evaluate elements of an integer array.

## Reported bugs

- Assignments can no longer shadow computed variables: `LINENO=999` and
  `BASHPID=1234` succeed and change nothing, while `SHELLOPTS` and
  `BASHOPTS` are readonly (reported by Gustav-Simonsson).
- A nameref chain deeper than the limit resolves to nothing with a
  warning, rather than silently returning a half-resolved name (reported
  by Gustav-Simonsson).

## New tunables

`$GNASH_NAMEREF_MAX` bounds nameref chain depth; it defaults to 100
where bash is fixed at 8.  `$BASHLY_CORRECT=true` saves that setting and
pins it to bash's value, restoring it when switched off.  The new
`strict-bash` personality is exactly that: the bash personality with the
switch turned on.
