#!/usr/bin/env bash
# conformance_gate.sh GNASH TESTDIR
#
# Regression gate over the bash tests/ files gnash currently passes *fully*
# (byte-identical to bash's .right).  Fails if any of them regress.  Grow the
# list as more of the bash suite goes green.
set -u

gnash=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
dir=$(cd "$2" && pwd)

export THIS_SH="$gnash"
PATH="$dir:$PATH"; export PATH

# bash tests/ files that gnash reproduces exactly.
passing="strip invert"

fails=0
for base in $passing; do
  [ -f "$dir/$base.tests" ] && [ -f "$dir/$base.right" ] || continue
  out=$(cd "$dir" && perl -e 'alarm 8; exec @ARGV' "$gnash" "./$base.tests" </dev/null 2>&1)
  if ! diff -q "$dir/$base.right" <(printf '%s\n' "$out") >/dev/null 2>&1; then
    echo "REGRESSION: bash test '$base' no longer matches" >&2
    fails=$((fails + 1))
  fi
done

if [ $fails -eq 0 ]; then
  echo "conformance gate: $passing pass"
  exit 0
fi
exit 1
