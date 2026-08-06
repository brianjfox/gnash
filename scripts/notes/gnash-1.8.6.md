# gnash 1.8.6

A large conformance sweep against bash 5.3's test suite: `glob`,
`builtins`, `varenv`, `posixexp2`, `dynvar`, and `herestr` now pass
byte-for-byte, and major clusters land in arithmetic subscripts,
command/heredoc scanning, function semantics, and associative-array
enumeration. 50 of the 83 bash test files now produce byte-identical
output (46 before this release).

## Pathname expansion complete (glob.tests -> 0)

- Posix 2.13.3: an unquoted `/` inside `[...]` invalidates the bracket
  expression, so `[qwe/qwe]` is not a pattern at all and survives
  nullglob untouched; a backslash in unquoted expansion data quotes the
  following character for the matcher (`var='a\?'; echo ${var}` is not
  a glob even with a file `a?` present).
- Non-pattern components are dequoted before filesystem use:
  `./tmp\/a/b/*` resolves through `tmp`, and `*/\.` matches
  `searchable/.`.
- `$GLOBSORT` is implemented: `[+-]?(name|size|mtime|atime|ctime|blocks|
  numeric|nosort)`, with bash's tie-breaking and unknown-keyword rules.
- `shopt -s failglob` reports `no match: PATTERN` once and aborts the
  command with status 1.
- ANSI-C `\u`/`\U` escapes encode via the active locale like bash's
  u32cconv: Big5 bytes under `LC_ALL=zh_TW.big5`, the literal
  `\uXXXX` text when the charset cannot represent the character, UTF-8
  otherwise.  The lexer and `read` consume multibyte characters
  atomically, so a Big5 trail byte 0x5C is never misread as a backslash.

## Builtins complete (builtins.tests -> 0)

- dirs/pushd/popd handle `--` (with bash's quirk that `popd -- +8` acts
  as plain `popd`, while `pushd -- +1` is a chdir to `./+1`).
- `. file args` keeps the sourced script's positional parameters when
  the script itself runs `set` at top level.
- `exit status` reports `numeric argument required` and the shell
  continues with status 2; `set -o -B` prints the option listing and
  then parses `-B`.
- `enable -n` hides a builtin from `type` and dispatch; `enable -d` on
  a static builtin fails with `not dynamically loaded`.
- `hash -p` rejects directories, `hash -lt` prints the reusable form,
  and `shopt -s checkhash` re-searches $PATH for stale entries.
- ulimit sets both limits by default, validates numbers (`+1999` is an
  `invalid number`), and names the resource in setrlimit failures.
- `help NAME` prints full long documentation, `help -m` renders the
  man-page layout, and every builtin accepts `--help` (except echo,
  test/[, :, true, false — where the word is data), matching bash.

## Function semantics (func.tests 35 -> 2)

- `wait` with no operands returns 0.
- posix-mode special-builtin prefix assignments write through an
  enclosing call tempenv: `var=30 func` where func runs `var=20 return`
  keeps 20 (export/readonly included; the default-mode promotion still
  unwinds).
- `<(:) ()` is rejected as `not a valid identifier`; posix-mode
  definition of a special-builtin-named function is fatal.
- Posix command lookup finds special builtins before functions, with
  `type`/`type -a` reporting accordingly.
- All-digit function names print bare (`11111 () `); `readonly -f`
  listings append `declare -fr NAME`.

## Variables and scoping complete (varenv.tests -> 0)

- Associative arrays enumerate in bash's hash-table order everywhere,
  with per-table bucket counts: fresh `declare -A` 1024, a converted
  scalar 128 (keeping its old value at key "0"), hashed commands 256,
  aliases 64.  BASH_CMDS, BASH_ALIASES, and the `hash` listings follow.
- `s=X; s+=(Y)` keeps the scalar as element 0; compound assignment to a
  readonly variable reports; localvar_inherit conversion failures print
  bash's two-line diagnostic.
- `shopt -s localvar_unset` and `set -a` (allexport) are implemented;
  an unexported invisible local no longer blocks the exported global in
  children's environments; bare `local` lists the current scope; and
  `readonly -p` includes `declare -r SHELLOPTS="..."`.

## Scanning and arithmetic (earlier in this cycle)

- `$((...))` vs `$( (...)` disambiguation follows bash's paren-depth
  rule; heredocs are tracked inside `$( )` in both the lexer and the
  expander, including `EOF)`-abutting delimiters and `<<-` tab-stripped
  delimiters (comsub-posix 91 -> 41, heredoc 57 -> 41).
- Arithmetic subscripts are expanded once, with bash's display-escape
  markers, raw-word provenance for `assoc_expand_once` builtin targets,
  and the full bad-subscript/identifier diagnostic matrix
  (arith 89 -> 7, quotearray 95 -> 27).
- POSIX tempenv persistence, value-stack unset, and `local -` option
  snapshots complete the varenv scoping model (varenv 84 -> 0 overall).
