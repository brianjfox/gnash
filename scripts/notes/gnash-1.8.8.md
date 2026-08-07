# gnash 1.8.8

`quotearray` joins the byte-perfect list and the command-substitution
parser is rebuilt to match bash's recursive model, taking
`comsub-posix` from 41 diff lines to 5 and `heredoc` from 41 to 17.
52 of the 83 bash test files now produce byte-identical output.

## Command substitutions parse like bash's (comsub-posix 41 -> 5)

- `$( )` content is now parse-validated recursively while the outer
  command is parsed, as bash does.  A construct left open inside a
  substitution is a syntax error of the outer parse: `echo $( if x;
  then echo foo )` reports ``near unexpected token `)'``, and a
  definite inner error appends ``while looking for matching `)'``.
- The lexer's substitution scanner is the single source of truth for
  where a span ends (`comsub_span_end`), so the parser and the
  expander agree: `$(case x in in|esac) ...;; esac)` no longer
  truncates at the pattern's `)`.
- `esac` closes an open `case` even out of command position, except as
  a pattern alternative; a `<<` inside `$(( ))` is the arithmetic
  left-shift, not a here-document.
- A backslash ending a comment is not a line continuation.

## Here-documents across the substitution boundary (heredoc 41 -> 17)

- A here-document left pending when a `$( )` closes on the same line
  takes its body from the lines after the full command: `echo $(cat <<
  EOF)` followed by the body emits it, with bash's `command
  substitution: 1 unterminated here-document` warning.
- `<<<` here-strings are consumed whole by both scanners, so multiline
  `$( cat <<< x )` works.
- The end-of-file warning names the line where input ended, prints even
  when the surrounding construct fails to parse, and `unexpected end of
  file from ... on line N` uses file coordinates.

## Array subscripts that survive one expansion (quotearray -> 0)

- `unset` re-expands an associative subscript only when the raw word
  quoted the brackets (`unset 'a[$var]'`); with unquoted brackets the
  word expansion already produced the final subscript.
- `[[ ]]` arithmetic comparators and `${string:off:len}` offsets
  evaluate their operands as arithmetic with one-shot subscript
  expansion, like `(( ))`, so keys holding `]`, `[`, or `$(...)`
  resolve; `[[` errors carry bash's `[[: ` prefix.
- `test` dispatches on argument count like bash: with exactly three
  arguments the middle must be a binary operator, so a word-split
  element reference is `binary operator expected` with status 2.
- `test -v` re-expands an associative subscript (assignment keeps the
  literal key), and `@`/`*` are literal keys for associative arrays.

## Redirections, traps, and POSIX expansion

- `exec 0< file` while reading from standard input rebinds the command
  source; a redirection failure on a compound command runs the ERR trap
  and is fatal under `set -e`; ERR-trap inheritance is decided at fork
  time, so a trap set inside a subshell fires.
- `$!` is unset until an asynchronous job runs; a `$((...))` syntax
  error unwinds the command list and is fatal in POSIX mode; a shell
  invoked as `sh` runs with POSIX semantics.

## Build and infrastructure

- Portability fixes for Debian/glibc builds (contributed by
  Gustav-Simonsson): a misleading-indentation split, a shadowed
  variable, a missing `<cstring>`, an explicitly typed initializer list
  for `uid_t`/`gid_t`, and `memcpy` where the length is exact.
- The repository's CLA status check no longer stalls pull requests:
  maintainer and bot pull requests take a fast path, the action is
  capped at ten minutes, and superseded runs are cancelled.
