# gnash 1.9.5

Mostly namerefs, and mostly cases where a diagnostic named the wrong
thing -- or where a variable quietly disappeared and the complaint
surfaced somewhere else entirely.  bash's `nameref` tests are down from
17 differing lines to 3.

## Namerefs

- `unset -n NAME` removed the variable when NAME was **not** a nameref.
  `-n` names a nameref to remove; finding none, it should stop rather
  than fall back to a plain `unset`.  The damage showed up at a distance:
  a variable destroyed here made a later `typeset -n` on it succeed where
  it should have been rejected, and the error moved to another line with
  another message.
- `${!ref[sub]}` on a nameref expanded to the last background pid.  The
  subscripted form fell through to the generic `${...}` path, where a
  leading `!` reads as `$!`.  It now indirects through that element of
  the target array: the element's value is the name to expand, and an
  element naming nothing is `NAME[sub]: invalid indirect expansion`.
- Resolving a nameref evaluates its subscript, so a `set -u` failure
  inside one is reported against *that* variable -- once.  Previously an
  unset base array lost the failure entirely and the nameref was blamed;
  an existing one reported it twice.

## Diagnostics naming the right thing

- Under `set -u`, an unset array element is named by the subscript as
  **written**: `a=() k=; "${a[k]}"` reports `a[k]`, not the `a[0]` it
  evaluated to.
- A failed assignment through `declare`/`readonly`/`typeset` now names
  whoever performed it.  An unquoted `NAME=(...)` is a compound
  assignment *word* of the command, which bash performs before entering
  the builtin -- so the enclosing function answers, and nothing does at
  top level.  A quoted compound is the builtin's own assignment, so the
  builtin answers.  Without an explicit `-a`/`-A` there is no
  attribution at all.
- `exec` names itself when its own `{var}` redirection cannot assign;
  the same redirection on a compound command stays unattributed.

## Other

- `cd -` printed its destination before attempting the change, so a `cd -`
  to a directory that had since vanished reported the error *and* printed
  the name.  The echo belongs to the change succeeding.
- An interactive shell now writes its history before `exec` replaces the
  process image.  History is saved when the shell exits, and `exec` never
  reaches that path, so a session's history was simply lost.
