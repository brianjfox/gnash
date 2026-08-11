# gnash 2.0.3

A job-control and parameter-expansion correctness release, plus complete
`help` documentation.

## Fixes

- Suspending a foreground command with ^Z printed the `[1]+  Stopped`
  notice twice: once when the stop was reaped and again before the next
  prompt.  The job is now marked as reported when the notice is printed,
  so it appears exactly once, as in bash.
- A job stopped from the foreground never became the *current job*: it
  had no `+` mark in `jobs` output, and the current-job specs (`%`,
  `%%`, `%+`, bare `fg`/`bg`) failed with `no such job`.  A stopped job
  now becomes the current job, matching bash.
- `${var@op}` accepted any operator letter (and `${var@}` with none);
  bash requires exactly one known transform (`U u L Q E P A a K k`) and
  reports `bad substitution` otherwise.  A command or arithmetic
  substitution in parameter-name position (`${$(cmd)}`, `${$((expr))}`)
  is likewise a bad substitution, not the PID.
- Malformed `${!prefix*}` / `${!prefix@}` name-listing forms — a digit
  prefix (`${!1*}`), a doubled special (`${!@*}`), or trailing junk —
  were accepted or mis-read as indirect expansion; they are now `bad
  substitution`, as bash reports.
- Under `set -u`, an unbound braced positional is reported as `N:
  unbound variable` (bash names only the bare `$N` form with a `$`).
- A negative array-slice length reported its evaluated value; the
  diagnostic now quotes the length expression as written
  (`$(($# - 2)): substring expression < 0`), matching bash.

## Improvements

- `help NAME` (and `NAME --help`, `help -m NAME`) now prints the full
  long-format documentation for every builtin and shell-syntax topic,
  matching bash 5.3 verbatim across all shared topics.
- `help variables` additionally documents gnash's own tunables:
  `$BASHLY_CORRECT`, `$GNASH_ENV`, `$GNASH_NAMEREF_MAX`,
  `$GNASH_PERSONALITY` and `$GNASH_VERSION`.
