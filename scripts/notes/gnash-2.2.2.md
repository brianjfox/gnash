# gnash 2.2.2

A patch release: the prompt no longer erases output that ended without a
newline, and four arithmetic and parsing fixes bring `$(( ))` closer to
bash.

## Line editor

- Output without a trailing newline (`cat` of a file whose last line is
  unterminated, `printf` without `\n`) stayed on the screen in bash but
  vanished under gnash, which repainted the prompt's row from column 0.
  The line editor now asks the terminal where the cursor is before the
  first paint and puts the prompt right there, as readline does; editing
  and line wrapping account for the offset. Terminals that do not answer
  the cursor-position query fall back to the old behavior after one
  500 ms wait per session (#695).

## Arithmetic

- `base#digits` literals are scanned the way bash's `strlong` does:
  `08#1` is rejected as "value too great for base", `010#1` and `0#1`
  as an invalid number, and `1#0` or `65#1` as an invalid base, instead
  of silently evaluating (#693).
- The true branch of `?:` is a full comma expression, so
  `1 ? 2 , 3 : 4` is 3 rather than a syntax error (#692).
- Whether `$((` opens arithmetic or the command substitution `$( (cmd) )`
  is decided at expansion time as bash does, with nested substitutions,
  quotes and backslashes treated as opaque. `$(( $(echo "(1" ) ))` is now
  arithmetic and fails with bash's "missing )" (#694).
- A `#` inside `$(( ))` never starts a comment, so
  `$(( $(echo 1)#1 ))` no longer swallows the closing `))` and fails to
  parse (#699).

## Install

```
brew tap brianjfox/tools && brew trust brianjfox/tools && brew install gnash
```

Or download the macOS tarball for your architecture (arm64 for
Apple Silicon, x86_64 for Intel) below.
