# gnash 2.0.0

A conformance release.  gnash now reproduces GNU Bash 5.3's own test
suite byte-for-byte across **79 of 83 test files** — the differences that
remain total 22 lines, down from roughly two thousand.  Two whole
families that had been stubborn for months went to zero in this cycle:
the internationalization tests (1827 differing lines → 0) and the
shell-invocation tests (58 → 0).

The version crosses to 2.0 to mark that milestone.  Nothing here breaks a
script that ran under 1.9 — every change moves gnash *closer* to bash,
never away — but the shell is a different animal than it was at 1.x: it
now handles Unicode, locales, coprocesses, shell startup, and here-document
edge cases the way bash does, not merely approximately.

## Internationalization and Unicode (`intl.tests`: 1827 → 0)

- `printf`, `printf %b`, and `echo -e` now decode `\uHHHH` / `\UHHHHHHHH`
  through the active locale's charset, exactly as bash's `u32cconv`:
  UTF-8 in a UTF-8 locale, an `iconv` conversion into the locale's
  encoding otherwise, and the ISO C99 escape text when a code point
  cannot be represented.  This alone accounted for the great majority of
  the differences.
- The full UTF-8 encoding ladder, including the historic five- and
  six-byte forms and the empty result for code points at or above
  `0x80000000` (`$'\Uffffffff'` is the empty string, as in bash).
- `printf %ls` / `%lc` — wide-character conversions whose field width and
  precision count characters, not bytes, and pad with spaces.
- `LC_NUMERIC` is honored, so `LANG=de_DE.UTF-8 printf '%.4f' 1` prints
  `1,0000`; a temporary locale (`LANG=C printf ...`) is torn down cleanly
  afterward instead of leaking into the rest of the script.
- Field splitting advances by whole characters, so a multibyte `IFS`
  never splits a character down the middle; and `${x##pattern}` falls
  back to byte matching when the value or pattern is not valid multibyte
  text.
- Command and `cd` error messages render an unprintable name in ANSI-C
  form (`$'5\247@...': command not found`), bash's `printable_filename`.

## Shell invocation (`invocation.tests`: 58 → 0)

- `SHELLOPTS`, `BASHOPTS`, and `BASH_ARGV0` are imported from the
  environment at startup, so a child shell inherits its parent's options
  (with invocation flags still overriding), and an exported `SHELLOPTS`
  serializes its live value to grandchildren.
- `set -B` / `set -o braceexpand` is now a real, switchable option —
  brace expansion can actually be turned off.
- `--pretty-print` is implemented.
- A script operand with no slash is searched for on `$PATH`, a binary
  file is rejected with `cannot execute binary file`, and a `#!`
  interpreter that does not exist is reported as
  `bad interpreter: No such file or directory`.
- A login shell runs `~/.bash_logout` on the way out (with the gnash
  persona's own fallback), and the usage banner names the shell as it
  was actually invoked.

## Here-documents and command substitution

- An alias whose body opens a here-document no longer breaks: the
  end-of-alias space bash withholds while a here-document is being read
  is withheld here too, so `alias h='cat <<EOF ... EOF'` works.
- The `here-document ... delimited by end-of-file` warning names the line
  that triggered gathering, matching bash.
- A command with more than sixteen here-documents is the fatal
  `maximum here-document count exceeded` bash reports.
- Commands inside `$(...)` report the enclosing command's line, as bash
  does since it parses substitutions up front.

## Coprocesses (`coproc.tests`: 11 → 0)

- Coprocess pipe descriptors are allocated high (below 64) exactly as
  bash's `sh_openpipe` does — `COPROC=(63 60)` for the first coproc —
  and are closed when the coprocess is reaped, so descriptors no longer
  leak.  A coprocess that dies immediately is torn down before the next
  command runs.

## `type`, `command`, and `hash`

- The command hash table now carries hit counts, `type` and `command -v`
  consult it, and `command -v` reports aliases and hashed paths.
- Function bodies printed by `type` / `declare -f` show `$'...'` and
  `$"..."` in bash's decoded, re-quoted form.

## Redirection, `read`, and pipelines

- A pipeline whose shell has stdin closed no longer breaks (the pipe's
  read end could land on fd 0 and be closed out from under the stage).
- `read` reports a hard `read(2)` failure instead of treating it as EOF,
  and re-checks its `-t` deadline on an empty end-of-file read.
- A bad substitution in a here-document body aborts the command, as any
  redirection failure does.

## Smaller fixes

- `select` validates its loop variable before installing its own line, so
  an invalid-identifier error is reported against the right line, and a
  function body's start line is installed on entry.
- A nameref to an array element re-expands its subscript (and re-runs any
  command substitution in it) on every dereference.
- `set -x` traces compound array assignments, and quoted parentheses
  without `-a`/`-A` are treated as a literal value, not a compound
  assignment.

The remaining 22 differences live in four test files
(`comsub-eof`, `comsub-posix`, `comsub2`, `exportfunc`) and are all
parser line-number or parse-error artifacts — some of them behaviors
bash's own source comments flag as bugs it intends to fix.
