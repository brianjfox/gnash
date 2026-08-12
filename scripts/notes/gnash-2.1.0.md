# gnash 2.1.0

A parameter-expansion and parser correctness release: pattern
substitution gains the `&` replacement feature, `${!prefix}` indirection
and the `@`-transforms are brought fully in line with bash, and several
long-standing arithmetic, `printf`, and command-substitution edge cases
now match bash 5.3 byte-for-byte.

## New

- Pattern substitution supports `patsub_replacement`: an unquoted `&` in
  the replacement of `${var/pat/rep}` stands for the matched text (`&`
  is a literal when the option is off or when backslash-escaped), as in
  bash.

## Fixes

### Parameter expansion and indirection

- `${!prefix*}` / `${!prefix@}` name listings now accept any prefix
  whose first character is a valid name-start; a non-identifier tail
  (`${!a.*}`) lists nothing instead of reporting `bad substitution`.
  The listings also carry full `$@`/`$*` field semantics — a quoted
  `@` keeps one field per name, a quoted `*` joins on `IFS`.
- `${!ref}` indirection now composes correctly with the `@`-transforms
  and with subscripts, and honors default/alternate operators and the
  special parameters.
- The `${var@A}` / `${var@a}` transforms report the complete attribute
  set, and `${@@A}` reconstructs the positional parameters.  Their
  interaction with `set -u` matches bash.
- `${N=word}` is rejected on a positional parameter, and an `[@]`/`[*]`
  slice of a scalar no longer mis-expands.

### Substitute words and command substitution

- An unquoted `$@` / `${a[@]}` splat inside a substitute word
  (`${x-$@}`) is space-joined, and a quoted `"$@"` keeps its fields, as
  bash does.
- Process substitution now works inside a substitute word.
- A double-quoted backquote substitution unescapes `\"` so the inner
  command sees real quotes.
- An unterminated `$(` in a here-document body reports bash's
  `command substitution: … unexpected EOF while looking for matching
  \`)'` instead of being left literal.
- A function substitution `${ …; }` executes its body's canonical
  rendering, so `$LINENO` and error lines inside a `for`/`while`/`if`
  body report the same line as bash.

### Parser

- A bare `in` can no longer start a command (`in`, `time in`,
  `echo x | in`), matching bash's `syntax error near \`in'`; a stray
  `done` correctly ends an open `case` inside a command substitution.
- The funsub `${ cmd; }` closing `}` is recognized only in command
  position, so `${ echo x }` (no terminator) reports the unterminated
  `}` instead of running.
- `(( a; b ))` stays arithmetic (and reports the arithmetic syntax
  error) instead of falling back to a subshell.
- The line reported for an unterminated `eval` construct, and for a
  missing redirection target at end of input, now matches bash.
- Exported-function import refuses a `BASH_FUNC_…` payload whose body
  defines a differently-named function.

### Arithmetic and printf

- `$((0x))` / `$((0X))` with no hex digits evaluate to `0`; a digit
  that is invalid for the base (`0xg`, `08`, `0x1p2`) reports `value
  too great for base` naming just the number token, as bash does.
- `printf` implements `%Q` (shell-quote with width/precision) and the
  `%a` / `%A` hex-float conversions, and `%b` with a bare `\x` emits a
  literal `\x` with a warning rather than a NUL byte.

### Globbing, prompt, and variables

- Pathname expansion and pattern substitution honor `nocaseglob` /
  `nocasematch`.
- Prompt expansion handles octal escapes, the `promptvars` option, the
  non-printing editing markers, `\s`, and POSIX `!` in `@P`.
- The dynamic special variables (`RANDOM`, `SECONDS`, `LINENO`, …) are
  removed permanently on `unset` and are no longer shadowed by a
  `declare` with no value, matching bash.
