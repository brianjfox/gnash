# gnash 1.4.2

A parser-conformance release. The shell grammar is brought closer to bash 5.3
in both directions — rejecting the malformed compound commands bash rejects,
while correctly parsing and reconstructing valid function definitions that the
stricter checks initially disturbed. Driven by a new negative-syntax
differential harness measured against `bash -n`.

## Syntax rejection (matching `bash -n`)

- An **empty command list** in a compound command is now a syntax error, as in
  bash: `if then fi`, `while do done`, an empty then/else part, and an empty
  `do` body all report `near unexpected token` with the offending reserved word.
- A **function body must be a compound command**: `f() echo hi`, `f() :`,
  `f() coproc x`, and similar now report `near unexpected token`, matching bash.
  Brace groups, subshells, `if`/`for`/`while`/`until`/`case`/`select`, `(( ))`,
  and `[[ ]]` bodies are accepted.

## Function definitions and here-documents

- **K&R function definitions** — the head on one line and `{ … }` on the next —
  parse correctly when a script is read line by line. (A regression in the new
  body check made the shell abandon such a definition and leak its body to the
  top level; with a here-document body, the leaked loop could run away.)
- **`declare -f` / `declare -pf`** now preserves the here-document body of an
  `if`/`while` condition (`while read v <<HERE … HERE do … done`), reproducing
  the function byte-for-byte instead of dropping the body.

## Testing

- Adds a **negative-syntax parser differential** (`tests/harness/gen_neg_corpus.sh`
  + `run_diff_corpus_neg.sh`): a corpus of malformed snippets compared against
  `bash -n` to measure rejection agreement. gnash now rejects 68/68 of them —
  the complement of the positive corpus's accept-agreement on real scripts.
