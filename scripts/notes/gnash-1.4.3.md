# gnash 1.4.3

A bash-conformance release driven by the bash 5.3 test-suite scoreboard: four
fixes that bring several test files to byte-for-byte agreement and sharply
reduce the diff on others.

## Command substitutions containing a `case`

- A `$( … )` (or `` `…` ``) command substitution whose body contains a `case`
  statement is no longer truncated at the `case` pattern's `)`. Both the lexer
  and the expander now recognize `case … esac`, so `$(case x in x) esac)` and a
  `case` inside a command substitution parse correctly. This also fixes a `case`
  used in an arithmetic-`for` header.

## Pattern substitution

- The replacement in `${var/pat/rep}` now undergoes full quote removal —
  independent of whether the enclosing `${…}` is double-quoted — so single
  quotes, double quotes, and a backslash before any character are all processed
  (`"${w/z/\a}"` → `a`, `"${v/\'/\'}"` → `'`, `'ab'` → `ab`), matching bash.

## Error messages

- Assigning `BASH_ARGV0` sets `$0` but no longer changes the program name shown
  in error messages: bash reports errors against the source file, so an error
  after `BASH_ARGV0=x` still names the script, not `x`.
- An arithmetic-`for` header keeps the source whitespace before a section's `;`
  when reproducing the expression in an error (`for (( 7=4 ; … ))` reports the
  section as `7=4 `, matching bash).

## Conformance

Against bash 5.3's own test suite, this release brings `dynvar`, `nquote`,
`iquote`, and `arith-for` to byte-for-byte agreement and roughly halves the
`comsub`/`comsub2` diffs.
