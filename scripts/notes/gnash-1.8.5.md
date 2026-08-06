# gnash 1.8.5

A conformance sweep against bash 5.3's test suite: alias expansion is
rewritten to bash's textual-splicing model, `alias`, `globstar`, and
`vredir` now pass byte-for-byte, and major clusters land in arithmetic
evaluation and redirection handling. 46 of the 83 bash test files now
produce byte-identical output (43 before this release).

## Alias expansion rewritten (bash's textual splicing)

Aliases are now expanded by splicing the body **textually** into the input,
exactly as bash's lexer does, instead of substituting tokens:

- an unbalanced quote in an alias body continues into the following text
  (`alias foo="echo 'Error:"` then `foo bar'` prints `Error: bar`);
- a body ending in an unquoted backslash escapes the next input character;
  a body of `#` comments out the rest of the line;
- bash's end-of-alias space, self-recursion guard, and ends-in-blank
  "expand the next word" chaining (with per-replacement re-checking) are
  reproduced, including the post-bash-5.2 multi-level behavior;
- case patterns and for/select variables are never expanded; in default
  mode the alias check precedes reserved-word recognition, in posix mode
  reserved words win, and posix mode enables aliases non-interactively;
- `-c` strings are parsed command-by-command, so an alias defined on one
  line is live on the next; completeness checks see the expanded text;
- a non-interactive shell stops reading input after a top-level syntax
  error (status 2), like bash — but not for errors inside `eval`, `source`,
  or trap bodies;
- alias names are validated (`alias '\$'=xx` is an error), and
  `BASH_ALIASES[name]=value` actually defines an alias.

## Arithmetic evaluation

- Read-modify-write operators evaluate an array subscript exactly once, so
  `(( dice[RANDOM%6+1 + RANDOM%6+1]++ ))` draws exactly two values and
  `$RANDOM` sequences match bash's generator stream.
- A bare array name reads and writes element 0 (`x=(1 2); ((x=9))`).
- Integer literals wrap on overflow like bash (`$(( -9223372036854775808 ))`).
- `set -u` aborts on an unset variable in arithmetic; value recursion stops
  at bash's limit; a malformed variable value aborts the expression with
  bash's diagnostic naming the value.
- `$(( 'foo' ))` is an arithmetic syntax error (single quotes are ordinary
  characters in arithmetic; double quotes are removed).
- Substring offsets understand ternary colons (`${PARAM:1 ? 4 : 2}`) and
  expand `$var`/`${var:-d}` offsets and lengths first.
- `(( ))` traces post-expansion text under `set -x`, and `declare -f`
  reproduces the raw expression spacing.

## Redirections

- Fixed a descriptor-management bug that broke every `exec N>file` in a
  script: when `open()` returned the target fd itself, the shell closed the
  descriptor it had just installed.
- Redirect backups moved out of the user-reachable fd range: previously
  `{ exec 10>&1; } > file` clobbered the block's stdout backup, leaving
  stdout pointing at the file forever (a later `cat file` grew without
  bound).
- `{var}` redirections now persist after normal commands (closed only under
  `shopt -s varredir_close`), accept array-element targets
  (`exec {fd[0]}<&0`), honor readonly variables, close the source in the
  move form, and report bash's exact diagnostics with the unexpanded word
  (`$fd: Bad file descriptor`).

## Globbing

- `globstar` keeps one result per `**` decomposition (`**/a/**` over
  `a/a/a` yields it three times, matching bash and ksh93), and a trailing
  `**/` lists symlinks to directories without descending them.
- Brace ranges no longer hang at the integer limits
  (`{9223372036854775805..9223372036854775807}`).

## Builtins

- `umask` symbolic modes accept who-copy permissions (`umask g+u`, `o=u`).
- POSIX-mode `.`/`source` searches `$PATH` only — no current-directory
  fallback — and reports `.: NAME: file not found`, fatal for a
  non-interactive posix shell; `command .` strips that fatality.
- Quoted function names (`'a b c' ()`) and non-identifier coproc names
  (`coproc @ { :; }`) parse and are rejected at execution, as bash does.
