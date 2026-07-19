# gnash 1.3.7

A large `bash`-conformance release. Since 1.3.6, gnash has been run against
`bash` 5.3's own test suite and brought into byte-for-byte agreement across
many areas. Highlights below; dozens of smaller fidelity fixes are included.

## Namerefs (`declare -n`)

Namerefs are now close to bash's behavior end to end:

- Target validation: invalid targets (`declare -n r=/`, `r="a b"`, an empty
  target) are rejected with bash's exact diagnostics and leave no half-created
  variable.
- Circular references: a self reference at function scope warns and resolves at
  global scope (`maximum nameref depth` on write, `circular name reference` on
  read); at global scope a direct self reference is still an error.
- `unset -n` removes the nameref itself; a temporary assignment to a nameref is
  scoped to its command; a nameref used as a `for` loop variable retargets each
  iteration; `${!ref}` yields the referenced name.
- Array interactions: a nameref can target an array element (`declare -n r=a[2]`);
  making an existing array a nameref is rejected; `declare -a` on an unset
  nameref converts it to a real array.

## Arrays and `declare` / `typeset`

- `declare -p` filters by attribute when given no names, lists by attribute,
  fixes flag order and associative-array formatting, and iterates associative
  arrays in bash's hash order.
- `+X` attribute removal, `name+=value`, `-u`/`-l`/`-c` case folding, and
  arithmetic evaluation of integer array-element assignments.
- Rejects converting an array between indexed and associative, assigning a list
  to a single element, and a subscripted target for `readonly`/`export`.

## Traps and signals

Implemented the `ERR`, `RETURN`, and `DEBUG` traps (including firing for a
function body on entry under `functrace` and for self-installed traps), the
`SIGCHLD` trap on child reap, `$BASH_TRAPSIG`, bash-format `trap` listing with
`-p`/`-P`/`-l`, ignored-signal listing, `kill -l`, and `$PS4` xtrace expansion.

## Job control, expansions, and globbing

- Correct `&` / `;` precedence, chained `&` as separate jobs, `time`,
  `lastpipe`, and `$PIPESTATUS`.
- Brace expansion: sequences with increments and zero-padding, nested and
  unmatched braces, escaped/quoted commas.
- Parameter and arithmetic expansion: `${@}`/`${*}` lists, `${#@}`,
  per-element operators, `$[...]`, arithmetic error messages, `$'...'`
  ANSI-C quoting inside `${...}` and here-document defaults, case-toggle
  operators.
- Globbing: `dotglob`/`extglob`, `globskipdots`, `GLOBIGNORE`, and keeping
  `.`/`..` out of `dotglob` matches.

## Builtins and readline

Rewrote `getopts` to match bash, improved `type`, `pushd`/`popd`/`dirs`,
exported functions via the environment, restricted-shell mode, POSIX mode from
`POSIXLY_CORRECT`, `set -k`, and `$SHELLOPTS`/`$-` option state. The `read`,
`history`, and `fc` builtins and the default emacs/vi keybindings now match
bash on its own test suites.

## Install

```
brew tap brianjfox/tools && brew trust brianjfox/tools && brew install gnash
```

Or download the universal (arm64 + x86_64) macOS tarball below.
