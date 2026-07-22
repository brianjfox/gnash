# gnash 1.3.9

A continued `bash`-conformance release. Since 1.3.8, gnash has been run
against further batches of `bash` 5.3's own test suite and brought into
byte-for-byte agreement across debugger support, programmable completion,
functions, and array subscripts. Highlights below; a number of smaller
fidelity fixes are included.

## Debugger support, DEBUG/RETURN traps, and `$LINENO`

- `$LINENO` inside a `DEBUG`/`RETURN`/`ERR` trap body reports the line of the
  command that fired the trap, not the trap body's own line.
- A function invoked while the `DEBUG` trap is running (including the trap
  handler itself) no longer spuriously fires the `RETURN` trap.
- The whole-body `DEBUG` trap reports a function's definition line, command
  substitutions run at their enclosing line, and a sourced file gets its own
  line numbering starting at 1.
- The `DEBUG` trap fires for each expression of an arithmetic `for (( ; ; ))`
  loop, and the `caller` builtin now matches bash (top-level form, option and
  invalid-number diagnostics).

## Programmable completion

- `complete -p` reconstructs each specification in bash's canonical option and
  action order with correct quoting, listed in bash's hash-table walk order.
- `compgen` gains the `setopt`, `shopt`, `enabled`, and `helptopic` action
  generators, the `-V varname` and `-X filterpat` options, and bash's
  option/action/identifier diagnostics; `compopt` validates `-o` option names.

## Functions

- Enforce `FUNCNEST` to cap function-call recursion.
- Reject a function name containing an unquoted `$`, and prefix `function ` to a
  non-identifier name in `declare -f`.
- Report an error for a malformed integer-attribute assignment (`i=0#4`).

## Arrays

- Array and associative subscripts are scanned by a single quote-aware parser,
  so a key containing `]` — escaped (`a[x\]y]`), quoted (`a["p]q"]`), or from a
  substitution (`a[$(cmd)]`) — is handled consistently for assignment, `${...}`
  reads, and compound literals.
- Implement `shopt -s array_expand_once`: an already-expanded array subscript is
  not re-expanded, so a command substitution left in it errors rather than
  running — closing a subscript-injection vector across `printf -v`, `read`,
  `unset`, assignment, arithmetic, `${a[sub]}`, and `test -v`.
- Implement `test -v` / `[ -v ]` for scalars and array elements.

## Install

```
brew tap brianjfox/tools && brew trust brianjfox/tools && brew install gnash
```

Or download the universal (arm64 + x86_64) macOS tarball below.
