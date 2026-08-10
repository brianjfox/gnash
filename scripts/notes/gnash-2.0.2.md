# gnash 2.0.2

A `printf`/`echo` correctness release.

## Fixes

- `printf '%(DATEFMT)T'` treated every negative argument as the current
  time.  Only `-1` (also the default when no argument is given) is the
  current time and `-2` is the shell's start time; every other value —
  including other negatives — is a literal `time_t`, so
  `printf '%(%Y-%m-%d)T' -86400` now formats `1969-12-31`.
- `printf` ignored the `l` length modifier (and `h`/`L`/`j`/`z`/`t`) on
  numeric and floating conversions: `%ld`, `%lf`, `%lx`, `%lu`, and the
  rest printed the literal format string instead of formatting the
  argument.  They now format correctly.  `%q` (shell-quote) and the wide
  `%ls`/`%lc` forms are unaffected.
- `printf "%c" ""` produced no output; it now emits a single NUL byte,
  as bash does (a missing argument is treated the same way).
- `echo -e '\x'` with no following hex digit emitted a NUL byte; it now
  prints the literal `\x`, matching bash.

With thanks to **Gustav-Simonsson**, who reported all four.
