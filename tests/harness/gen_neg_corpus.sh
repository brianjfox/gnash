#!/usr/bin/env bash
# Copyright (c) 2026 Brian J. Fox
# Licensed under GPLv2 with the GPLv2-AI Exception.
#
# gen_neg_corpus.sh  >  neg_corpus.nul
#
# Emit a NUL-delimited corpus of deliberately MALFORMED shell snippets -- inputs
# a correct parser must reject.  The positive parser differential
# (run_diff_corpus_parse.sh over real scripts) measures parity on valid input;
# this corpus measures the other half: does gnash-parse reject the syntax errors
# that `bash -n' rejects?  Snippets are NUL-delimited (some span multiple lines).
set -u
emit() { printf '%s\0' "$1"; }

# --- unterminated compound commands ------------------------------------------
emit 'if true; then echo hi'
emit 'if true; then echo hi; fi; if false'
emit 'while true; do echo x'
emit 'until false; do echo x'
emit 'for i in 1 2 3; do echo $i'
emit 'for i in 1 2 3'
emit 'case x in a) echo a;;'
emit 'case x in'
emit 'select x in a b; do echo $x'
emit '{ echo hi'
emit '( echo hi'
emit '(( 1 + 2'
emit '[[ 1 -eq 1'
emit 'echo $(echo hi'
emit 'echo ${x'
emit 'echo `echo hi'

# --- keyword / reserved-word misuse ------------------------------------------
emit 'if then fi'
emit 'for do done'
emit 'while do done'
emit 'then echo hi'
emit 'else echo hi'
emit 'elif true; then echo'
emit 'fi'
emit 'done'
emit 'esac'
emit 'do echo hi'
emit 'if true fi'
emit 'case x in y) echo y esac'
emit 'if true; then; fi'
emit 'for; do echo; done'

# --- operators in the wrong place --------------------------------------------
emit '| echo hi'
emit '&& echo hi'
emit '|| echo hi'
emit 'echo hi |'
emit 'echo hi &&'
emit 'echo hi ||'
emit 'echo a | | echo b'
emit 'echo a ; ; echo b'
emit 'echo a &&& echo b'
emit ';'
emit ';; echo'
emit 'echo ;;'
emit 'echo a |& |& echo b'

# --- redirections ------------------------------------------------------------
emit 'echo hi >'
emit 'echo hi > > file'
emit '< '
emit 'cat < < f'
emit 'echo hi 2>&'
emit 'echo hi >&'

# --- function definitions ----------------------------------------------------
emit 'f() echo hi'
emit 'function'
emit 'f() { echo hi'
emit '() { echo hi; }'
emit 'f(x) { echo; }'

# --- conditional errors ------------------------------------------------------
# (arithmetic content errors like $(( 1 + )) are runtime, not parse-time, errors
# in bash -- omitted here so every entry is one bash actually rejects at parse.)
emit '[[ -z ]]'
emit '[[ 1 -eq ]]'
emit '[[ 1 = = 2 ]]'

# --- subshell / group mismatches ---------------------------------------------
emit 'echo hi )'
emit '} echo hi'
emit ') echo hi'
emit '{ echo hi )'
emit '( echo hi }'

# --- quoting / here-doc ------------------------------------------------------
emit 'echo "unterminated'
emit "echo 'unterminated"
emit 'echo $'"'"'unterminated'
emit 'cat <<'
emit 'a=(1 2 3'
emit 'declare -A m=([k]=v'
