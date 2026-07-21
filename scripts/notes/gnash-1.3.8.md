# gnash 1.3.8

A continued `bash`-conformance release. Since 1.3.7, gnash has been run
against further batches of `bash` 5.3's own test suite and brought into
byte-for-byte agreement across arrays, builtins, and control flow.
Highlights below; a number of smaller fidelity fixes are included.

## Arrays and `declare`

- Unset individual array elements, and diagnose bad and readonly
  array-element assignments.
- Parse a quoted compound array value in `declare`, and create an indexed
  array from a subscripted `declare` name.
- Slice an indexed array by index rather than dense position, and drop empty
  elements from an unquoted array expansion.
- Reject bad subscripts in a compound array literal and a bare value mixed
  into an associative-array literal; quote associative-array subscripts in
  `declare -p` and `set` output.

## Special and virtual variables

- Expose the special virtual arrays in `declare`/`set` listings, and list
  `BASH_ALIASES` and `BASH_CMDS` as associative arrays.
- Treat a declared-but-unset variable as unset for `-v`.

## Builtins

- `umask`: `-S`, `-p`, and symbolic modes.
- `cd`: search `CDPATH`.
- `source`: `source -p PATH`, honor the `sourcepath` shopt, pass positional
  parameters to a sourced file, and fix its error prefix.
- `enable -s` filters to the POSIX special builtins.
- Keep prefix assignments to `export`/`readonly`/`declare -x` permanent.
- `shopt` reports an error for an invalid option or `-o` name, and
  `emacs`/`monitor`/`privileged` are recorded as `set -o` option state.

## Control flow and scripts

- Propagate `continue N` through enclosing loops.
- Re-exec a script with no `#!` line using the current shell.

## Signals

- List signals in columns, and add `SIGEMT`, `SIGIO`, and `SIGINFO`.

## Install

```
brew tap brianjfox/tools && brew trust brianjfox/tools && brew install gnash
```

Or download the universal (arm64 + x86_64) macOS tarball below.
