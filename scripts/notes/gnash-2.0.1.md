# gnash 2.0.1

A small portability release.

## Fixes

- `core/src/parser.cpp` used a `std::` algorithm without including
  `<algorithm>`, which builds on some toolchains only by accident (a
  transitive include) and fails on others.  The include is now explicit.
- `Shell::set_personality` renamed an array-name loop variable from `nm`
  to `var` so it no longer shadows an outer name, clearing a `-Wshadow`
  warning.

With thanks to **Gustav-Simonsson**, who contributed both fixes.
