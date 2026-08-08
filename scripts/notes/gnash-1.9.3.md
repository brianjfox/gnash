# gnash 1.9.3

A release about line numbers.  Everything here is in service of the same
thing: a script that traces its own control flow -- with `set -T`, a
DEBUG or RETURN trap, `$LINENO`, `caller` -- now sees the lines bash
sees.  bash's `dbg-support` tests are down from 59 differing lines to 8.

## Traps

- A `\`-newline continuation inside a compound command shifted every
  line after it up by one.  A function whose body joined two lines
  reported `$LINENO` one short from there on, and so did the DEBUG trap
  and `BASH_LINENO`.  Top level was never affected, because each command
  starts its own count; a compound command's body shares one.
- The DEBUG trap fired only for the commands inside a compound, never
  for the compound's own header, so a trace lost every `for` and `case`
  line.  It now fires for the command itself -- once for a `case`, and
  once per iteration for a `for`, so the header reappears each time
  round.  A standalone `(( ))` fired none at all.
- `$LINENO` on the first line of a RETURN trap body reported 1 rather
  than the line that triggered the trap.  DEBUG already had this right.
- The traps that fire when a function body ends reported whatever line
  the last command inside it had reached.  bash names the function's
  definition line again -- the same one its entry DEBUG trap reported.

## source

A sourced file is a function frame to bash: leaving one runs the RETURN
trap, reporting the line `source` was called on.  gnash ran the file and
returned silently, so a script tracing its own control flow lost every
`source` boundary.  `$FUNCNAME` inside that trap names the caller, as
bash does.
