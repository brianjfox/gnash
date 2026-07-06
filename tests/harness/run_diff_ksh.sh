#!/usr/bin/env bash
# Copyright (c) 2026 Brian J. Fox
# Licensed under GPLv2 with the GPLv2-AI Exception.

# run_diff_ksh.sh GNASH KSH
#
# Differential execution for gnash's ksh personality: run each script under
# gnash (in its ksh persona) and under a reference ksh93, comparing stdout and
# exit status.  gnash's ksh persona rides the shared Bourne engine, so these are
# common ksh/POSIX constructs that must produce identical results.
#
# GNASH should be a `ksh'-named symlink to the gnash binary, or the gnash binary
# itself (then run with --personality=ksh).
set -u

gnash=${1:?usage: run_diff_ksh.sh GNASH KSH}
ksh=${2:-/bin/ksh}

tmp=$(mktemp /tmp/gnash_ksh.XXXXXX)
trap 'rm -f "$tmp"' EXIT

# Hard wall-clock cap so a runaway script can't hang the suite.
cap() { perl -e 'alarm 8; exec @ARGV' "$@"; }

run_gnash() {
  case $(basename "$gnash") in
    ksh|ksh93) cap "$gnash" "$tmp" </dev/null ;;
    *)         cap "$gnash" --personality=ksh "$tmp" </dev/null ;;
  esac
}

scripts=(
  'echo hello world'
  'x=5; y=3; echo $((x + y)) $((x * y)) $((x % y))'
  'echo $(( 2 ** 10 ))'
  'echo $(( 17 / 5 )).$(( 17 % 5 ))'
  'for i in 1 2 3; do echo "n=$i"; done'
  'for ((i=0;i<3;i++)); do printf "%d " $i; done; echo'
  'i=0; while [ $i -lt 3 ]; do echo w$i; i=$((i+1)); done'
  'i=1; until [ $i -gt 3 ]; do echo u$i; i=$((i+1)); done'
  'case abc in a*) echo match;; *) echo no;; esac'
  'n=7; if ((n>5)); then echo big; else echo small; fi'
  # parameter expansion / string operators
  'v=hello; echo ${#v} ${v%%l*} ${v#he} ${v/l/L}'
  's=abcdef; echo ${s:2:3}'
  'x=42; echo ${x} ${x:0:1}'
  'v=path/to/file.txt; echo ${v##*/} ${v%.*}'
  'unset u; echo "[${u:-default}]"'
  'echo "quote: ${var:-none}"'
  'echo ${PWD:+set}'
  # arrays (0-based, like bash) and typeset attributes
  'a=(one two three); echo ${a[0]} ${a[2]} ${#a[@]}'
  'a=(5 3 8 1); echo ${a[1]} ${a[3]}'
  'arr=(a b c); arr+=(d); echo ${arr[@]} ${#arr[@]}'
  'typeset -i n=10; n=n+5; echo $n'
  'typeset -u U=hello; echo $U'
  'typeset -l L=HELLO; echo $L'
  # arithmetic commands and let
  'x=5; ((x+=3)); echo $x'
  'let "z = 6 * 7"; echo $z'
  # positional params, shift, IFS splitting
  'set -- a b c; echo $# $1 $3; shift; echo $1'
  's="a,b,c"; IFS=,; set -- $s; echo $1 $2 $3'
  # tests and [[ ]]
  'test -z "" && echo empty; test -n "x" && echo nonempty'
  '[[ foo == f* ]] && echo glob; [[ bar != f* ]] && echo notf'
  # functions and return status
  'f() { echo "arg=$1"; return 2; }; f hi; echo "rc=$?"'
  # brace expansion, printf, pipes, command substitution, here-string
  'echo {1..4}'
  'printf "%s-%d\n" abc 7'
  'printf "%05.2f\n" 3.14159'
  'echo abc | tr a-z A-Z'
  'r=$(echo hi | wc -c); echo $((r))'
  'echo a b c | while read w; do echo ">$w"; done'
  'read -r a b <<< "x y z"; echo "[$a][$b]"'
)

fails=0
for s in "${scripts[@]}"; do
  printf '%s\n' "$s" > "$tmp"
  g_out=$(run_gnash 2>/dev/null); g_rc=$?
  k_out=$(cap "$ksh" "$tmp" </dev/null 2>/dev/null); k_rc=$?
  if [ "$g_out" != "$k_out" ] || [ "$g_rc" != "$k_rc" ]; then
    echo "MISMATCH: $s" >&2
    echo "  gnash (rc=$g_rc): $g_out" >&2
    echo "  ksh   (rc=$k_rc): $k_out" >&2
    fails=$((fails + 1))
  fi
done

if [ $fails -eq 0 ]; then
  echo "run_diff_ksh: all ${#scripts[@]} scripts match ksh"
  exit 0
fi
echo "run_diff_ksh: $fails mismatch(es)" >&2
exit 1
