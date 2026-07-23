# gnash 1.4.0

A continued `bash`-conformance release. Since 1.3.9, gnash has been run against
further batches of `bash` 5.3's own test suite and brought into byte-for-byte
agreement across function substitution, arithmetic conditionals and loops,
parameter expansion, arrays, here-documents, and command substitution. The
headline addition is support for `bash` 5.3's `${ command; }` function
substitution. Highlights below; many smaller fidelity fixes are included.

## Function substitution (`${ command; }`)

- Implements `bash` 5.3's `${ command; }` funsub — command substitution that
  runs in the current shell without a subshell, so side effects persist — and
  the `${| command; }` valsub, whose value is the command's `$REPLY`.
- A funsub containing a function definition or group command
  (`${ f() { echo hi; }; }`) now parses: nested braces balance as a command
  list rather than being cut short by an inner `}`.
- A funsub body runs in a fresh local scope (so a `local` inside it does not
  leak) and is a return boundary: `return` ends the funsub rather than unwinding
  the enclosing script.

## Arithmetic conditionals and loops

- The `(( ))` command and each section of an arithmetic `for` loop now report a
  malformed expression with bash's `((: EXPR: ...` diagnostic instead of failing
  silently, and a dangling `++`/`--` reports the second sign as the error token.
- An arithmetic `for` loop with empty sections — `for (( ;; ))`,
  `for (( i=0;;i++ ))` — parses correctly, aborts on an arithmetic error in any
  section instead of spinning forever, and prints an omitted section as `1`.
- The `[[ ]]` numeric comparators (`-eq`, `-lt`, ...) evaluate each side as a
  full arithmetic expression (`[[ 7 -eq 4+3 ]]` is true) and report a bad one
  with `[[: EXPR: ...`; `[[ -t FD ]]` is implemented and rejects a non-integer
  file descriptor with `integer expected`.

## Parameter expansion and arrays

- `${a[@]@k}` and `${a[@]@K}` expand an array to its key/value pairs — as
  separate words, or as one requoted `key value ...` string — matching bash's
  quoting for indexed and associative arrays.
- Anchored pattern substitution `${var/#prefix/repl}` and `${var/%suffix/repl}`
  replaces only a matching prefix or suffix.
- A negative array subscript counts from the end on both read and element
  assignment (`${x[-1]}`, `x[-2]=v`), with `bad array subscript` when out of
  range.
- Unquoted and assignment `${a[*]}` / `$*` join their elements with the first
  character of `IFS`, like the quoted form.
- Single quotes stay literal inside a double-quoted `${x-...}`-style operator
  word.

## Here-documents and command substitution

- A backslash-newline inside a quoted here-document (`<<'EOF'`) is kept literal;
  an unquoted here-document still splices it before the delimiter check.
- A command substitution nested in a parameter expansion —
  `${foo:-$(echo a{b,c})}` — is scanned by paren balancing, so its inner braces
  no longer corrupt the surrounding `${...}`.

## Builtins

- `help` prints the two-column topic listing in bash's `dispcolumn` layout and
  includes the special-form entries (`if`, `for (( ))`, `[[ ... ]]`, `time`,
  `coproc`, and the rest).
- `hash NAME` reports `not found` for an unhashed name, `unset` falls back to
  removing a function, and `declare -f NAME` reports `not found` for a missing
  function.
- `$UID` and `$EUID` are defined as readonly integer variables.

## `test` / `[`

- The `test` builtin's `-v` unary operator reports whether a variable (or array
  element) is set.
