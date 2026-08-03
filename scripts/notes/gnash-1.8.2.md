# gnash 1.8.2

A small name-reference conformance fix.

## Name references

- A nameref whose target is an array element (`declare -n ref=a[0]`) can no
  longer be subscripted further: `ref[foo]=x` would mean `a[0][foo]`, so bash
  rejects the resolved target with `` `a[0]': not a valid identifier ``. gnash
  previously created a variable literally named `a[0]`. Subscripting a nameref
  to a whole array (`declare -n ref=arr; ref[2]=x`) is unaffected.
