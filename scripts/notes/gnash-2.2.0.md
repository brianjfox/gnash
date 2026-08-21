# gnash 2.2.0

A minor release: the `printf` builtin now reproduces bash 5.3's printf
test suite byte for byte — new conversions, `*` field widths, full
argument and format validation, and bash-exact diagnostics — and
`brew install gnash` reliably pours the bottle again on the release
machine.

## Features

- `printf` gains `*` field widths and precisions, taken from the
  argument list exactly as bash's `getint` does (#675): a negative
  width left-justifies, a negative precision is treated as missing,
  a leading `'` or `"` yields a character code, and out-of-range or
  malformed values diagnose (`Result too large` / `invalid number`)
  while processing continues.
- `printf` implements `%n` (#665): the named variable receives the
  byte count written so far in the current format pass, following
  namerefs, with bash's identifier validation and readonly semantics.
- `%S` and `%C` are recognized as bash's synonyms for the wide
  `%ls`/`%lc` in multibyte locales (#681).

## Fixes

- A pathological wide-conversion field width (`%100000000000ls`) no
  longer attempts a gigabytes-large padding allocation — it diagnoses
  `Result too large` and continues, as bash does (#666).
- Numeric conversions validate their arguments (#680): `printf '%d'
  GNU` prints `0` with `invalid number` and status 1, out-of-range
  values clamp with a diagnostic, `%u` takes the full uintmax range,
  floats accept character codes, and multibyte character codes decode
  the whole character (`printf '%d' "'À"` is 192) (#686).
- Missing and invalid format characters are diagnosed and stop
  processing (`%10`, `%z`, `%M`, `%y`), still writing the output
  accumulated so far (#681).
- Option handling matches bash (#682): a missing format or unknown
  option prints the usage line with status 2, `-vNAME` works attached,
  and the `-v` target is validated before any conversion runs.
- `%q`/`%Q` follow bash's printstr path (#675, #686): `%q` precision
  truncates the quoted string while `%Q` pre-truncates the raw
  argument, `%#q` always single-quotes, and only `-` is honoured among
  padding flags.
- `%b`, `%c` and `%(fmt)T` honour field width and precision (#678),
  and `%b`/`echo -e` expand `\e` (#686).
- `%(fmt)T` matches bash throughout (#688): `export TZ=...` (and
  `unset TZ`) take effect immediately, an empty format means `%X`,
  the closing paren is found by balanced scan, an invalid spec warns
  and echoes literally, the seconds argument is validated, and
  `LC_ALL`/`LC_TIME` now drive strftime's locale.
- printf's stdout and stderr interleave as bash's line-buffered output
  does, so diagnostics land after the completed lines that preceded
  them (#686).
- `test '(' '' ')' -a b` was reported as a wrong result (#670); it is
  deliberate bug-for-bug compatibility with bash 5.3's `posixtest()`,
  now documented in the source.

## Build

- `scripts/release.sh` finishes by reinstalling gnash from the bottle
  it just published, so the release machine no longer keeps a
  `--build-bottle` from-source install that made every later
  `brew install`/`upgrade` compile from source (#671).

## Install

```
brew tap brianjfox/tools && brew trust brianjfox/tools && brew install gnash
```

Or download the macOS tarball for your architecture (arm64 for
Apple Silicon, x86_64 for Intel) below.
