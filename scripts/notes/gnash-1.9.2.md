# gnash 1.9.2

Two more of bash's test files now pass byte-for-byte -- `comsub` and
`cond` -- bringing the total to **62 of 83**.  Conditional expressions
account for most of the work, and most of it turned out not to be about
error messages at all.

## Conditional expressions

`[[ ]]` gained four behaviours it was missing outright:

- The file comparisons `-nt`, `-ot` and `-ef` were accepted by the
  parser but never evaluated, so every one of them answered true --
  whatever the files were, or whether they existed.  They now share the
  `test` builtin's rules: a file that cannot be stat'd is older than one
  that can, and `-ef` compares device and inode.
- `&&` and `||` now short-circuit.  An arm that cannot change the answer
  was still being evaluated, so a command substitution in it ran and an
  error in it was reported: `[[ -n a || -t X ]]` failed with
  `X: integer expected` where bash returns 0.
- Only bash's fixed set of unary operators is recognized, so `-Q` is an
  operand rather than an operator and `[[ -Q 7 ]]` is an error instead
  of being silently accepted.
- A failing conditional (and a failing `(( ))`) fires the ERR trap, as a
  simple command does.

Syntax errors are now reported as bash reports them -- the specific
diagnostic, `syntax error near` the token, and the offending source line
-- including an unterminated `[[` at end of input, which pairs its
diagnostic with the grammar-level report on the following line.

Under `set -x` a conditional is traced one term at a time, so
`[[ -n a && -n b ]]` prints two lines, grouping parentheses never appear,
and the implicit `-n` of a bare `[[ x ]]` does.

## Traps

`$BASH_COMMAND` inside a trap body reported the trap's own command
rather than the one that triggered it, because each command the body ran
overwrote it.  It now stays fixed at the triggering command for the whole
body, as bash does.

## Command substitutions

An alias whose body supplies the closing `)` no longer swallows the rest
of the line: `alias short='echo ok 8 )'` followed by `echo $( short` on
its own line used to absorb the next command and print it as an
argument.

## Expansions

- A negative length in `${var:offset:length}` counts from the end of the
  value, so `${v:0:-5}` drops its last five characters.  An end that
  falls before the offset is `substring expression < 0`, and an array
  slice rejects a negative length outright.
- `${}` is a `bad substitution`, while `${ }` remains a function
  substitution with an empty body.
- `read -a` and `mapfile` evaluate the elements of an integer array.

## Arithmetic

A deeply nested expression no longer fails at 1000 levels with a syntax
error about a stray token: the bound is sized against the stack, charging
parenthesised levels by what they actually cost, and reports
`expression recursion level exceeded` when it is reached.
