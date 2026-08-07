# gnash 1.9.0

Seven more of bash's test files now pass byte-for-byte -- `array`,
`braces`, `complete`, `procsub`, `quote`, `quotearray`, and `read` --
bringing the total to **58 of 83**.  Command substitutions, funsubs,
and backslash quoting all move substantially closer to bash, and the
first externally reported bug is fixed.

## Command substitutions and funsubs

- Aliases are expanded while scanning `$( )` content, as bash's parser
  does: `alias switch=case` makes `$( switch x in y) ...;; esac )` scan
  and parse correctly, and an alias whose body carries unbalanced
  parentheses moves where the substitution ends (`alias nest='('`,
  `alias short='echo ok 8 )'`).  A balanced body is left for the
  runtime parse, so `alias let='let --'` still expands exactly once.
- A here-document left pending when a substitution closes takes its
  body from the lines after the whole command, with bash's
  `command substitution: 1 unterminated here-document` warning; `<<<`
  here-strings are consumed whole by both scanners.
- A `${ ...; }` funsub does not inherit `errexit` unless
  `inherit_errexit` or POSIX mode says so, and a `${| ...; }` valsub's
  `$REPLY` is private to the body.
- `set -o posix` turns `expand_aliases` on, and off again on exit.

## Process substitutions (procsub.tests -> 0)

`$!` names a process substitution's child and the child stays
waitable, so `cat <(exit 123) >/dev/null; wait "$!"` reports 123 --
including a `wait` issued after the creating command has finished.

## Quoting and expansion

- A `$(` starts a fresh quoting context, so a single quote inside it
  quotes again even within the enclosing double quotes.
- A `\` before a newline is a line continuation even at end of file,
  while a `\` that ends the input with no newline is literal:
  `sh -c 'echo escape\'` prints `escape\`.
- Brace character ranges yield literal characters -- a `$`, backquote,
  or quote produced by `{Z..a}` is never re-scanned as syntax, and the
  backslash element becomes an empty field that still occupies a word.
- Junk after an array subscript is a `bad substitution` error rather
  than being silently dropped (reported as issue #459 by
  Gustav-Simonsson; the same check fixes `${a[0]junk}` and
  `${a[0] + b[y]}`).
- Substring offsets and `[[ ]]` arithmetic operands evaluate with
  one-shot subscript expansion, like `(( ))`, so associative keys
  holding `]`, `[`, or `$(...)` resolve.

## Builtins and diagnostics

- `test` dispatches on argument count like bash: with exactly three
  arguments the middle one must be a binary operator, so a word-split
  element reference is `binary operator expected` with status 2.
- `unset` reports an invalid identifier or malformed element reference
  only under `-v`; plain `unset` stays silent.
- A reserved word in the wrong place is `near unexpected token`, which
  also gives substitution-validation errors their
  `while looking for matching ')'` suffix.
- A readonly `for` variable aborts the loop after one diagnostic; a
  readonly array assignment reports; `read -e` off a terminal fails at
  end of input instead of reporting a timeout; and `personality` is
  hidden from builtin listings, where bash has no such builtin.
