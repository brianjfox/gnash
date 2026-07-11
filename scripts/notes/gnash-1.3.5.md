# gnash 1.3.5

Bug-fix release over 1.3.4, improving `bash` fidelity in three areas.

## Fixes

- **`-n` (noexec) now works.** The flag was accepted but never acted on, so
  scripts ran instead of being syntax-checked. Commands are now parsed but not
  executed, honored through `-n`, `set -n`, `set -o noexec`, the `set -o`
  listing, and `$-` — and, like bash, ignored when interactive. A mid-script
  `set -n` takes effect for the commands that follow it.
- **ANSI-C `$'...'` numeric escapes.** Only single-letter escapes were decoded,
  so `\033`, `\x41`, `✓`, and friends leaked through as literal text. The
  quoting now decodes octal (`\nnn`), hex (`\xHH` and `\x{...}`), Unicode
  (`\uHHHH` / `\UHHHHHHHH`, encoded as UTF-8), control (`\cX`), and `\E` / `\?`,
  matching bash's `ansicstr`.
- **`!` inside single quotes.** A `!` in `'...'` was treated as a history
  expansion; it is now left literal, matching bash's
  `history_quotes_inhibit_expansion` default. (Double quotes still expand it.)

## Install

```
brew tap brianjfox/tools && brew trust brianjfox/tools && brew install gnash
```

Or download the universal (arm64 + x86_64) macOS tarball below.
