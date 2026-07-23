# gnash 1.4.1

A `bash`-conformance bug-fix release. Four divergences from `bash` 5.3 —
surfaced by a new corpus-scale differential harness — are fixed, each brought
into byte-for-byte agreement on stdout, exit status, and stderr.

## Parameter and array expansion

- `${!#}` and `${!N}` now perform indirect expansion through the special
  parameter `$#` and through a positional parameter: `set -- w x y z; echo ${!#}`
  prints `z`. Previously the `!` indirection only fired for identifier names, so
  `${!#}` was mis-parsed as `$!` and yielded `0`.
- The defaulting/alternative/error operators now work on a whole array:
  `${a[@]:-word}`, `${a[@]-word}`, `${a[@]:+word}`, `${a[@]:?word}` (and the
  `[*]` forms). An empty or all-empty array is treated as null, so
  `a=(); echo "${a[@]:-DEFAULT}"` prints `DEFAULT`. These were previously
  mis-parsed as a slice and produced nothing; `${a[@]:=word}` now reports bash's
  `bad array subscript` error.

## Associative arrays

- An unquoted space inside a compound-initializer subscript is kept as part of
  the key: `declare -A m=([a b]="c d"); echo "${m[a b]}"` prints `c d`.
  Previously the key was split and stored as `[a`.

## `printf`

- The `%(FMT)T` time conversion is implemented: the argument is taken as
  seconds since the epoch (`-1` or absent = now) and formatted through
  `strftime`, so `printf '%(%Y-%m-%d)T\n' 0` prints the epoch date instead of the
  literal format.

## Testing

- Adds corpus-scale differential harnesses under `tests/harness/`: a parser
  differential (`gnash-parse` vs `bash -n`) over real system scripts, and an
  execution differential over a generated, sandboxed snippet corpus. The
  execution corpus now agrees with `bash` 5.3 on 268/269 snippets; the four
  fixes above were found and verified with these tools.
