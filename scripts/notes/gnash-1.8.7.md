# gnash 1.8.7

The conformance sweep continues: `assoc` and `varenv` join the
byte-perfect list, `array` and `posixexp` come within a line or two,
and parser, redirection, and diagnostic behavior land major clusters.
51 of the 83 bash test files now produce byte-identical output.

## Associative arrays complete (assoc.tests -> 0)

- Key/value compound lists expand each word whole -- no word splitting,
  no pathname expansion (`declare -A v=( $foo 3 )` with `foo='1 2'`
  keys on "1 2"; an unquoted `*` stays a literal key); an empty key is
  `"": bad array subscript`.
- `unset` of associative elements takes the subscript to the last `]`
  (`unset A[]]` removes the key `]`), re-expands it without
  assoc_expand_once, and honors BASH_COMPAT <= 51's literal pre-5.2
  semantics -- `unset 'map[foo$(cmd)bar]'` never runs the command
  substitution.
- declare/typeset assignment words treat associative subscripts as
  literal keys under assoc_expand_once (`declare x["a[b"]=1` keys on
  `a[b`); read/printf reject targets whose subscript's first expansion
  left an unbalanced quote; `declare +A` reports `cannot destroy array
  variables in this way`.

## Indexed arrays and compounds (array.tests -> 1)

- `name=(...)` compounds are recognized only in assignment-acceptable
  positions; after an ordinary command word the `(` is bash's parse
  error (`printf "%s\n" -a a=(a 'b  c')`).
- Element-assignment subscripts validate before the readonly attribute;
  negative indices resolve against a set scalar's implicit element 0;
  a scalar assignment to an array variable writes element 0.
- The string-compound reparse matrix matches bash: expanded
  parenthesized values reparse as compounds with second-round element
  expansion and integer folding; subscripted `-a`/`-A` forms drop the
  subscript; unflagged subscripted forms store the literal string with
  bash's deprecation warning.
- `${a[-N]}` out of range reports and expands empty while `${#a[-N]}`
  aborts; `set -u` names unset elements (`narray[4]: unbound variable`);
  `$(( $@ ))` joins the positionals with spaces.

## POSIX expansion semantics (posixexp.tests -> 4)

- `$!` is unset until an asynchronous job runs; unset positionals error
  under `set -u`.
- A `$((...))` syntax error unwinds the whole command list, and in
  POSIX mode is fatal to a non-interactive shell (exit 127).  A shell
  invoked as `sh` runs with POSIX semantics.
- Inside `${...}` in a here-document, double quotes and backslash
  escapes are active while single quotes and `$'...'` stay literal.
- With extquote, `$'...'` in parameter-name position decodes to the
  name (`${$'x1'%$'t'}` is `${x1%t}`); a bare-quoted name is bash's
  `bad substitution` error.

## Redirections and traps (redir.tests 25 -> 19)

- `exec 0< file` while reading commands from standard input rebinds
  the command source (`${SH} < redir1.sub` continues in redir2.sub).
- A redirection failure on a compound command runs the ERR trap and is
  fatal under `set -e`; ERR-trap non-inheritance is modeled at fork
  time, so a trap set inside a subshell fires.

## Diagnostics (heredoc.tests 41 -> 31, comsub-eof 20 -> 10)

- The end-of-file here-document warning names the line where EOF was
  reached, prints even when the surrounding construct fails to parse,
  and `unexpected end of file from ... on line N` uses file
  coordinates.

