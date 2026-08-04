# gnash 1.8.3

A name-reference conformance release. `declare -n` references — especially
those that point at an array element or a whole-array splat — now behave much
more like bash across declaration, attribute, and expansion.

## Name references

- **A nameref to an array element no longer creates a phantom variable.**
  `declare -n ref=a[0]` followed by `declare -A ref` / `declare ref=` now
  applies attributes to the base array `a` and writes the element through the
  subscript, instead of creating a variable literally named `a[0]` (bash has no
  such variable — `declare -p a[0]` reports "not found").

- **A nameref to a whole-array splat expands to every element.**
  `declare -n ref=arr[@]` (and the indirect `indir=arr[@]; "${!indir}"`) now
  expand `$ref` / `"${ref}"` to all of `arr`'s elements with `${arr[@]}`
  field-splitting semantics, rather than collapsing to the first element.

- **`+=` through a nameref honors the target's integer attribute.**
  With `declare -ai a; declare -n b=a[0]`, `b+=1` now adds arithmetically
  (`4` → `5`) instead of string-appending (`"41"`).

- **`readonly` / `export` reject a nameref that resolves to an array element.**
  `declare -n ref=var[0]; readonly ref` now reports
  `` `var[0]': not a valid identifier `` and changes nothing, matching bash;
  `declare -r` / `declare -i` still convert the element's base to an array.

- **Removing attributes from a readonly targetless nameref works.**
  `declare -rn r; declare +n r` now yields `declare -r r`, and `+r` on such a
  reference is a silent no-op — bash forbids stripping the reference only when
  it actually points at something.

- **Retargeting a targetless nameref shows its new target.**
  `typeset -n r; typeset -n r=P` now prints `declare -n r="P"` rather than
  `declare -n r` (the value was set but hidden).

- **`declare -a NAME[sub]` on a nameref makes NAME the array.** It now warns
  `removing nameref attribute` and turns NAME itself into the array, instead of
  following the reference; and a quoted compound value on `declare -n`
  (`declare -n array='(one two three)'`) is rejected as an invalid target.

- **Writing through a nameref to `X[@]` / `X[*]`** now reports
  `X[@]: bad array subscript` instead of silently writing element 0.

- **`declare -p NAME…` output ordering** now interleaves found declarations and
  "not found" messages in argument order, matching bash under a pipe.
