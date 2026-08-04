# gnash 1.8.4

Implements the `select` builtin and continues the name-reference conformance
work.

## The `select` builtin

`select NAME in WORDS; do …; done` is now fully implemented. Previously it ran
as a plain `for` loop — iterating the words once each with no menu, prompt, or
input. It now behaves as bash does:

- prints a numbered menu of the words to standard error in bash's column
  layout (columns sized from `COLUMNS`);
- prompts with `$PS3` (default `#? `) and reads a line into `REPLY`;
- a blank line reprints the menu; a valid `1..N` choice sets `NAME` to that
  word; an out-of-range or non-numeric entry sets `NAME` to the empty string;
- runs the body after each choice and loops until `break` or end-of-input;
- binds `NAME` through the normal assignment path, so a name reference target
  or a readonly variable is honored.

## Name references

- **A targetless nameref cannot be subscript-assigned.** `declare -n ref;
  ref[0]=x` now reports `` `': not a valid identifier `` instead of creating a
  hybrid array.
- **`${!name}` with an empty target** (`name` unset or empty) now reports
  `name: invalid indirect expansion` and aborts the command, rather than
  re-parsing the operator text.
- **`declare -n` on an existing array is left untouched** when rejected: a
  declared-but-empty array still prints `declare -a array`, not `=()`.
- **Array-literal attributes apply through a nameref to its target.**
  `local -n ref=var; local -i ref=([1]=)` makes `var` an integer array (empty
  element → `0`) and leaves `ref` a plain reference.
- **Newly adding `-n` strips the integer/case attributes** (a reference cannot
  be integer), while re-declaring an existing reference keeps them.
- **A coproc command now reports its own line number** in diagnostics.
- **Writing through a nameref that references its own element** (`local -n
  a=a[0]; a=X`) is rejected as `` `a[0]': not a valid identifier `` after the
  circular-reference warnings.
