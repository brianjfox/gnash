# gnash 1.7.5

A conformance patch release sharpening name-reference (`declare -n`) diagnostics
and POSIX-mode error handling, matching bash byte for byte on more of the
`nameref` and `errors` test cases.

## Name references

- Assigning a value to a nameref that has no target yet stores the value as the
  referent *name*, so it is validated as an identifier rather than evaluated.
  `declare -n foo; declare -i foo=7*6` now reports `` `7*6': not a valid
  identifier `` (the raw text bash quotes) instead of the computed `42`.
- The invalid-target diagnostic carries the builtin that triggered it, as in
  bash: `((: `0'`, `printf: `/'`, `getopts: `?'` — where a plain `r=X`
  assignment stays bare. `getopts` also prints its `illegal option` /
  `option requires an argument` message before the nameref error, matching
  bash's ordering.
- An empty value assigned to a targetless nameref is rejected too:
  `declare -n r; r=""` (and `: ${r=}`) now report `` `': not a valid
  identifier ``.
- `for ref in WORDS` validates each word before retargeting the reference:
  `declare -n r; for r in /` reports `` `/': not a valid identifier `` and
  aborts the loop, as bash does, rather than silently pointing `r` at `/`.
- The nameref attribute can no longer be added to a pre-existing readonly plain
  variable: `declare -r RO=x; declare -n RO` reports
  `declare: RO: readonly variable`. A combined `declare -rn foo=bar` on a fresh
  variable, re-declaring an existing nameref, and `-x` on a readonly variable
  all still work.

## POSIX mode

- `-o posix` and `--posix` are now honored on the command line, not only via a
  runtime `set -o posix`.
- A POSIX special built-in that fails now exits a non-interactive shell, as
  POSIX requires: `sh -o posix -c 'readonly a=a; unset -v a; echo X'` stops at
  the `unset` error instead of printing `X`. This applies to `unset`, `.` /
  `source`, and `return`; the bash exceptions (`trap` with a bad signal,
  `shift`, `break`/`continue`) remain non-fatal. `unset` also now rejects an
  invalid identifier (`unset -v a-b`).
