# gnash 1.7.6

A drop-in-compatibility release: gnash now parses real-world scripts that
previously failed (including `config.guess` and Homebrew's `xcrun` shim), takes
its first steps toward multibyte-locale correctness, and ships a harness for
measuring gnash against bash on a corpus of real scripts.

## Parser fixes for real-world scripts

- A `'` in a comment (`# it's likely ...`) no longer opens a phantom
  single-quoted string when deciding whether a later line's trailing backslash
  is a real continuation. This made scripts such as `config.guess` — used by
  nearly every autotools build — fail with `syntax error: expected 'fi'`.
- The right-hand side of `[[ ... =~ RE ]]` now keeps whitespace inside a
  parenthesised group: `[[ $x =~ (^| )-show-sdk ]]` (as in Homebrew's `xcrun`
  shim) is one pattern again, tracking `(`/`)` depth so a blank ends the regex
  only outside a group — matching bash, `BASH_REMATCH` groups included.

## Multibyte / locale

- gnash adopts the environment's locale at startup (`setlocale(LC_ALL, "")`, as
  bash does) and re-applies `LC_CTYPE` when `LC_ALL`, `LC_CTYPE`, or `LANG` is
  assigned, so multibyte handling follows the locale.
- `${#var}` and the `${var:offset:length}` / `${var: -n}` substrings now count
  and slice by character, not byte: `${#café}` is 4 and substrings fall on
  code-point boundaries. ASCII and C-locale behavior are unchanged.

## Tooling

- `scripts/corpus-diff.sh` runs a corpus of real-world scripts under both gnash
  and bash and reports where they diverge, classifying a script one shell
  accepts and the other rejects as a real blocker versus a cosmetic difference.
  A parse-only default keeps it safe to run over arbitrary system scripts.
