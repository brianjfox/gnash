#!/usr/bin/env bash
# Copyright (c) 2026 Brian J. Fox
# Licensed under GPLv2 with the GPLv2-AI Exception.

# run_diff_zsh.sh GNASH ZSH
#
# Differential execution for gnash's zsh personality: run each script under
# gnash (in its zsh persona) and under a reference zsh, comparing stdout and
# exit status.  These scripts exercise the expansion behaviors where zsh
# diverges from bash -- word splitting and bare-array expansion -- so gnash's
# zsh persona must match real zsh exactly.
#
# GNASH should be a `zsh'-named symlink to the gnash binary, or the gnash
# binary itself (then run with --personality=zsh).
set -u

gnash=${1:?usage: run_diff_zsh.sh GNASH ZSH}
zsh=${2:-/bin/zsh}

tmp=$(mktemp /tmp/gnash_zsh.XXXXXX)
trap 'rm -f "$tmp"' EXIT

# Hard wall-clock cap so a runaway script can't hang the suite.
cap() { perl -e 'alarm 8; exec @ARGV' "$@"; }

run_gnash() {
  case $(basename "$gnash") in
    zsh) cap "$gnash" "$tmp" </dev/null ;;
    *)   cap "$gnash" --personality=zsh "$tmp" </dev/null ;;
  esac
}

# Each script prints results with visible delimiters so any word-splitting
# difference shows up in the compared output.
scripts=(
  # unquoted scalar does not word-split under zsh
  'v="a b c"; printf "[%s]" $v; echo'
  # double-quoted scalar is one word (same as bash)
  'v="a b c"; printf "[%s]" "$v"; echo'
  # unquoted command substitution does not word-split
  's=$(echo p q r); printf "[%s]" $s; echo'
  'printf "[%s]" $(echo x y z); echo'
  # bare array expands to all elements, one word each
  'a=(one two three); printf "[%s]" $a; echo'
  # double-quoted bare array joins elements with the first IFS char
  'a=(one two three); printf "[%s]" "$a"; echo'
  # elements that contain spaces stay intact (no secondary split)
  'b=("x y" z); printf "[%s]" $b; echo'
  # empty scalar is removed from an unquoted list
  'e=""; printf "[%s]" A $e B; echo'
  # a for-loop over a bare array iterates once per element
  'a=(one two three); for w in $a; do printf "<%s>" "$w"; done; echo'
  # a for-loop over an unquoted scalar iterates once (no split)
  'v="a b c"; for w in $v; do printf "<%s>" "$w"; done; echo'
  # non-whitespace IFS is not used to split unquoted expansions
  'IFS=:; p="1:2:3"; printf "[%s]" $p; echo'
  # braced array forms are unchanged
  'a=(one two three); printf "[%s]" "${a[@]}"; echo'
  'a=(one two three); printf "[%s]" "${a[*]}"; echo'
  'a=(one two three); echo ${#a[@]}'
  # ${var:-default} and other operators still work
  'echo ${nope:-fallback}'
  'v=hello; echo ${v/l/L}; echo ${#v}'
  # positional parameters keep their (bash-compatible) behavior
  'set -- p1 p2 p3; printf "[%s]" $@; echo; printf "[%s]" "$*"; echo'
  # 1-based array indexing
  'a=(alpha beta gamma delta); echo ${a[1]} ${a[2]}'
  'a=(alpha beta gamma delta); echo "[${a[0]}]"'
  # brace-free subscripts
  'a=(alpha beta gamma delta); echo $a[1] $a[3]'
  # negative indices count from the end
  'a=(alpha beta gamma delta); echo $a[-1] $a[-2]'
  # inclusive ranges, including negative bounds
  'a=(alpha beta gamma delta); printf "<%s>" $a[2,3]; echo'
  'a=(alpha beta gamma delta); printf "<%s>" $a[2,-1]; echo'
  'a=(alpha beta gamma delta); printf "<%s>" $a[-2,-1]; echo'
  # element count (${#a} and $#a) and scalar length
  'a=(alpha beta gamma delta); echo ${#a} $#a'
  'n=hello; echo $#n ${#n}'
  # 1-based writes
  'a=(alpha beta gamma delta); a[2]=BETA; printf "<%s>" $a; echo'
  # 1-based subscripts inside arithmetic
  'nums=(10 20 30); echo $((nums[1] + nums[3]))'
  # associative arrays are keyed by string, not translated
  'typeset -A h; h[key]=val; h[two]=2; echo ${h[key]} ${h[two]}'
  # zsh flat key/value associative assignment
  'typeset -A h; h=(one 1 two 2 three 3); echo ${h[two]} ${#h}'
  # ${(flags)...} expansion flags: join / split / newline
  'a=(foo bar baz); echo "${(j:-:)a}"'
  's="1:2:3"; printf "<%s>" ${(s.:.)s}; echo'
  'a=(foo bar baz); printf "<%s>" "${(F)a}"; echo'
  'l=$(printf "x\ny\nz"); printf "<%s>" ${(f)l}; echo'
  # sort (o/O), numeric (n), unique (u), and combinations
  'a=(foo bar baz); printf "<%s>" ${(o)a}; echo; printf "<%s>" ${(O)a}; echo'
  'n=(3 1 22 4); printf "<%s>" ${(no)n}; echo; printf "<%s>" ${(o)n}; echo'
  'd=(b a b c a); printf "<%s>" ${(u)d}; echo; printf "<%s>" ${(ou)d}; echo'
  # case flags L / U / C
  'v="Hello World"; echo ${(L)v}; echo ${(U)v}; echo ${(C)v}'
  # associative keys / values, sorted
  'typeset -A h; h=(one 1 two 2 three 3); printf "<%s>" ${(ok)h}; echo; printf "<%s>" ${(okv)h}; echo'
  # split flag on a command substitution
  's=$(echo "x+y+z"); printf "<%s>" ${(s:+:)s}; echo'
  # ${=name}: force word splitting, even inside double quotes
  'w="a b c"; printf "<%s>" ${=w}; echo; printf "<%s>" "${=w}"; echo'
  # scalar character indexing and substrings (1-based, negatives from the end)
  'str=hello; echo $str[1] $str[-1] ${str[3]}'
  'str=hello; echo $str[2,4] ${str[2,-1]}'
)

fails=0
for s in "${scripts[@]}"; do
  printf '%s\n' "$s" > "$tmp"
  g_out=$(run_gnash 2>/dev/null); g_rc=$?
  z_out=$(cap "$zsh" -f "$tmp" </dev/null 2>/dev/null); z_rc=$?
  if [ "$g_out" != "$z_out" ] || [ "$g_rc" != "$z_rc" ]; then
    echo "MISMATCH: $s" >&2
    echo "  gnash (rc=$g_rc): $g_out" >&2
    echo "  zsh   (rc=$z_rc): $z_out" >&2
    fails=$((fails + 1))
  fi
done

if [ $fails -eq 0 ]; then
  echo "run_diff_zsh: all ${#scripts[@]} scripts match zsh"
  exit 0
fi
echo "run_diff_zsh: $fails mismatch(es)" >&2
exit 1
