# gnash 1.7.7

A multibyte-correctness release: under a UTF-8 locale gnash now handles string
length, substrings, case conversion, pattern matching, word splitting, and
`read` by character rather than by byte, matching bash. It also fixes a
`[[ =~ ]]` parser case and ships tooling for measuring gnash against bash on a
corpus of real scripts.

## Multibyte / UTF-8

- Case conversion is character-aware: `${v^^}` / `${v,,}` / `${v^}` / `${v~}`,
  the `${v@U}` / `@L` / `@u` transforms, and the `declare -u` / `-l` / `-c`
  attribute fold multibyte letters correctly (`${x^^}` on `café` is `CAFÉ`, not
  `CAFé`), including array element values.
- Pattern matching counts characters: `?` matches one whole character, a
  bracket class such as `[[:alpha:]]` matches a multibyte letter, and the
  `${var#pat}` / `${var%pat}` removals fall on character boundaries — so
  `${x%?}` on `café` yields `caf`, `case résumé in r?sum?)` matches, and `????`
  globs a four-character name.
- Word splitting and joining use whole IFS characters: `IFS=€` splits `1€2€3`
  into three fields instead of on each byte of `€`, and `"$*"` / `"${a[*]}"`
  join with IFS's first character.
- `read -n N` / `read -N N` count characters, and `read` splits its input on a
  multibyte IFS character correctly.

A unibyte or C locale is unchanged throughout; existing ASCII behavior is
byte-for-byte identical.

## Parser

- The right-hand side of `[[ ... =~ RE ]]` ends the regex at a `)` that closes
  an enclosing conditional group — `[[ ! ( "$t" =~ ^(latest|stable)$ ) && ... ]]`
  now parses, while a genuine group inside the pattern is still kept.

## Tooling

- `scripts/corpus-diff.sh` gains a usable `--exec` mode that runs each script
  under both shells in an isolated environment and diffs stdout/stderr/exit,
  alongside the existing parse-only default. `scripts/corpus-fetch.sh` assembles
  a broad real-world corpus, and `scripts/probes/` holds deterministic feature
  probes that gnash matches byte-for-byte.
