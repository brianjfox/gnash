#!/usr/bin/env bash
# Copyright (c) 2026 Brian J. Fox
# Licensed under GPLv2 with the GPLv2-AI Exception.

# parse_diff.sh GNASH_PARSE BASH
#
# Differential parser check: every snippet below is in gnash's currently
# supported grammar subset, so gnash-parse and `bash -n` must agree on
# accept/reject.  Exit 0 if all agree, 1 (with a report) otherwise.
set -u

gp=${1:?usage: parse_diff.sh GNASH_PARSE BASH}
bash=${2:?usage: parse_diff.sh GNASH_PARSE BASH}

# Snippets bash and gnash should both ACCEPT.
accept=(
  'echo hello world'
  'a && b || c'
  'ls | grep x | wc -l'
  'a | b |& c'
  '! grep -q x file'
  'if true; then echo y; fi'
  'if a; then b; elif c; then d; else e; fi'
  'for i in 1 2 3; do echo $i; done'
  'for x; do echo $x; done'
  'while read x; do echo $x; done'
  'until false; do echo y; done'
  'case $x in a) echo 1;; b|c) echo 2;; *) echo 3;; esac'
  'f() { echo hi; }'
  '{ a; b; }'
  '( cd /tmp && ls )'
  'cat < in > out 2>&1'
  'echo hi >> log'
  'x=1 y=2 env'
  'echo a; echo b &'
  'echo "$(date +%s)" ${HOME}/bin'
  $'cat <<EOF\nhello\nEOF'
  $'cat <<-END\n\tindented\n\tEND'
  $'if true\nthen\n  echo hi\nfi'
  '[[ -f foo && -d bar ]]'
  '[[ $x == a* || $y != b ]]'
  '(( x = 1 + 2 ))'
  'for (( i=0; i<3; i++ )); do echo $i; done'
  'select opt in a b c; do echo $opt; done'
  'a=(1 2 3); echo ${a[0]}'
  '! grep -q x f'
  'time echo hi'
  'time ! echo hi'
)

# Snippets bash and gnash should both REJECT.
reject=(
  'if a; then b'
  'while a; do b'
  'for i in 1 2; do echo $i'
  'done'
  'fi'
  'a |'
  'a &&'
  '( a'
  'case x in a) b;;'
  'for; do x; done'
  '{ a;'
)

fails=0

for s in "${accept[@]}"; do
  printf '%s' "$s" | "$bash" -n 2>/dev/null; b=$?
  printf '%s' "$s" | "$gp" -n >/dev/null 2>&1; g=$?
  if [ $b -ne 0 ]; then continue; fi   # skip if this bash rejects (version drift)
  if [ $g -ne 0 ]; then
    echo "MISMATCH (should accept): $s" >&2
    fails=$((fails + 1))
  fi
done

for s in "${reject[@]}"; do
  printf '%s' "$s" | "$bash" -n 2>/dev/null; b=$?
  printf '%s' "$s" | "$gp" -n >/dev/null 2>&1; g=$?
  if [ $b -eq 0 ]; then continue; fi   # skip if this bash accepts
  if [ $g -eq 0 ]; then
    echo "MISMATCH (should reject): $s" >&2
    fails=$((fails + 1))
  fi
done

if [ $fails -eq 0 ]; then
  echo "parse differential: all snippets agree with bash -n"
  exit 0
fi
echo "parse differential: $fails mismatch(es)" >&2
exit 1
