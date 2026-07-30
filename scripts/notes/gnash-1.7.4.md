# gnash 1.7.4

A conformance patch release completing the arithmetic array-subscript
error-reporting cluster and implementing the `wait -n` job-selection framework,
matching bash byte for byte on more of the `array` and `jobs` test cases.

## Array subscript arithmetic errors

- An indexed or scalar array subscript is now expanded in a double-quoted
  context, exactly as bash does: single quotes stay literal and a bare `~` is
  not tilde-expanded. `${a[' ']}` and `${b[~]}` reach the arithmetic evaluator
  as `' '` / `~` and raise `arithmetic syntax error: operand expected`,
  aborting the command instead of silently reading element 0.
- A failed subscript assignment inside a builtin (for example
  `declare -i a[$bad]=x` under `array_expand_once`) no longer leaks its
  arithmetic-error state onto the following command.

## `wait -n` and job control

- `wait` now understands `-f`, `-n`, and `-p var`. `wait -n` returns after the
  next of the named jobs finishes (or any job when none are named), waiting for
  whichever completes first rather than in argument order; an unknown `%spec`
  reports `no such job` but the wait continues with the rest.
- `wait -p var` stores the finished job's pid in `var`; when no job is waited
  for, the variable is unset (a readonly target reports
  `cannot unset: readonly variable`). An invalid name reports
  `not a valid identifier`.
- `disown` now accepts `-a` (all jobs), `-r` (running jobs only), and `-h`
  together with any number of job specs — previously `disown -a` matched
  nothing and left finished jobs in the table.
