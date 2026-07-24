# gnash 1.4.4

A small bash-conformance release from the bash 5.3 test-suite scoreboard.

## Command substitutions

- A `$( … )` command substitution is no longer truncated at a `)` that appears
  inside a `#` comment. Both the lexer and the expander now skip comments while
  scanning, so `$(#comment )` closes on the following line's `)`, as in bash.

## The `let` builtin

- A leading `--` now ends option processing, so `let -- EXPR` evaluates `EXPR`
  and the common `alias let="let --"` idiom works (only the first `--` is
  consumed).
- `let` stops at the first expression that fails to evaluate and returns
  failure, matching bash: `let a=1 - b=2` sets `a` but not `b` and returns 1.

## Conformance

Continues to reduce the `comsub` / `comsub-posix` test-suite diffs against
bash 5.3.
