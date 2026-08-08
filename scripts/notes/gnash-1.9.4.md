# gnash 1.9.4

Four more of bash's test files now pass byte-for-byte -- `dbg-support`,
`func`, `arith` and `trap` -- bringing the total to **66 of 83**.  Most
of the work was in two areas that turned out to be one idea each: which
line a diagnostic belongs to, and which traps a new execution context
inherits.

## Line numbers in diagnostics

A message that names a line was often naming the wrong one.  bash keeps
its idea of the current line in three parts, and gnash had none of them:

- Only a handful of commands install a line at all -- a simple command,
  a subshell, `(( ))` and `[[ ]]`, and `for`/`select`.  `while`, `until`,
  `if`, `case`, a `{ }` group and a function definition deliberately do
  not.
- The line is *scoped*: it is saved, installed for the command, and
  restored when that command finishes, so what is in force is always the
  innermost enclosing construct that set one -- never a leftover from
  whatever ran last.
- Otherwise the line comes from the parser: whatever it last consumed.

So a redirection error on a multi-line `while ... done > file` names the
`done`, and one inside `( ... )` names the subshell's closing parenthesis
-- which is also the line a function definition rejected in posix mode is
reported against.

An unterminated quote, backquote or `${` now names the line it *opened*
on rather than the line input ran out on.  An unterminated `$(` still
names the end, which is bash's own inconsistency, not ours.

## Traps

- A sourced file had bash's inheritance rules backwards in both
  directions: it inherited the DEBUG trap when it should not, and never
  fired the RETURN trap when it should.
- A command substitution or subshell no longer inherits the DEBUG and
  RETURN traps.  For a command substitution this was visible in the
  *value*: with a DEBUG trap set, `x=$(echo hi)` came back as `"DBG hi"`,
  because the trap wrote into the capture pipe.
- The DEBUG trap fires for a `for` or `case` command itself, once per
  iteration for a `for`, and for a standalone `(( ))`.
- `trap SIGSPEC` with a single operand reverts that signal; a single
  operand naming no signal is a usage error rather than a silent no-op.
- A bare `return` in a signal trap action reports `$?` from before the
  trap ran -- the action cannot change it (POSIX interp 1602).
- A signal ignored when the shell started can be neither trapped nor
  reset.
- `declare -ft` marks a function traced, so it inherits the DEBUG and
  RETURN traps with `set -T` off.

## Tracing

`$BASH_XTRACEFD` is implemented: `set -x` output can go to a descriptor
other than stderr, and a value that is not an open descriptor is
reported.  `$PS4`'s first character now repeats once per nesting level,
so a trace from inside an `eval` or a trap action reads `++` -- while a
function call and a plain subshell, which do not nest, still read `+`.

## Arithmetic

An empty array subscript is diagnosed but is no longer fatal: a read
yields 0, a write is skipped while the assignment still evaluates to its
right-hand side, and the enclosing command list keeps running.  A
character that can begin neither an operand nor an operator is an
`invalid arithmetic operator` rather than a generic syntax error.  And
`assoc_expand_once` applies to indexed subscripts too, so
`let "a[\" \"]"=18` is a syntax error rather than a write to element 0.

## Other

`echo` reports a failed write.  This is not only about full disks:
duplicating a read-only descriptor onto standard output succeeds, so
`exec 3</etc/passwd; echo hi >&3` fails only when the write happens, and
that failure used to be discarded.
