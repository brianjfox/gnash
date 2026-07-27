# gnash 1.7.1

A conformance patch release focused on local-variable scoping and the
temporary environment, matching bash byte for byte on more of its `varenv`
and `array` test cases.

## Local-variable scoping

- **`localvar_inherit`** is now implemented: with `shopt -s localvar_inherit`,
  a `local`/`declare` inside a function inherits the value and attributes
  (integer, array, associative, exported) of the nearest enclosing variable of
  the same name instead of starting unset. A value given on the command still
  overrides the inherited one.
- The **`declare`/`local -I`** flag forces that same inheritance for a single
  command, regardless of the shopt.

## Temporary environment and assignments

- A temporary-environment assignment that a `declare -r` makes readonly is now
  promoted to a real, exported variable, matching bash: `x=4 declare -r x`
  yields `declare -rx x="4"` (previously the value was reverted). `declare -i`
  and a bare `declare`, which set neither readonly nor export, still do not
  persist.
- A scalar `declare` assignment to a name that is already an array targets
  element 0 — `declare a=0` on an existing array `a` now sets `a[0]` (as a plain
  `a=0` does) rather than an ignored scalar shadow. This also corrects several
  `array` test cases.

## Options

- **`ignoreeof`** is bound to `$IGNOREEOF`: `set -o ignoreeof` seeds
  `IGNOREEOF=10`, `set +o ignoreeof` unsets it, and the option's reported state
  tracks whether the variable is set.
- A named `shopt -o NAME` query pads the option name to the shopt column width,
  matching bash's per-context formatting.
