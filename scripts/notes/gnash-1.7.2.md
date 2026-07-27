# gnash 1.7.2

A conformance patch release refining function-local variable scoping, the
temporary environment, and the EXIT trap to match bash byte for byte on more of
the `varenv` test cases.

## Local variables and the temporary environment

- A `local` of an **exported** variable stays exported, so the environment a
  function passes to its child processes is unchanged.
- A variable passed in the **temporary environment** (`v=t f`) is inherited —
  value and exported attribute — by a `local`/`typeset` of the same name inside
  the called function: `v=t f; f(){ local v; }` yields `declare -x v="t"`, and
  `x=12 f; f(){ typeset x="${x-10}"; }` sees 12.
- `unset` of a variable **local to the current function scope** now leaves it
  declared-but-unset rather than removing it, so it keeps shadowing any
  enclosing value and a later assignment reuses the local (`local v=x; unset v;
  declare -p v` reports `declare -- v`).

## Traps

- The **EXIT trap** now runs with the exiting function's call frame still
  active, so `$FUNCNAME` names the function when `exit` is invoked from inside
  one: `trap 'echo $FUNCNAME' EXIT; f(){ exit; }; f` prints `f`.
