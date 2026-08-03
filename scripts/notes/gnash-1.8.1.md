# gnash 1.8.1

A small name-reference conformance fix.

## Name references

- `declare +n` / `typeset +n` on a readonly nameref now reports
  `NAME: readonly variable` and leaves the reference in place, as bash does --
  removing the nameref attribute would change a readonly variable's type. A
  `+n` on a readonly non-reference variable remains a harmless no-op, and a
  `+n` on a read-write nameref still removes the attribute.
