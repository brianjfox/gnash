#!/usr/bin/env bash
# Copyright (c) 2026 Brian J. Fox
# Licensed under GPLv2 with the GPLv2-AI Exception.

# run_diff_csh.sh GNASH TCSH
#
# Differential execution for gnash's csh personality: run each csh script under
# gnash (invoked so it takes on its csh persona) and under a reference tcsh,
# comparing stdout and exit status.  These scripts are all within the csh
# interpreter's implemented feature set, so they must match exactly.
#
# GNASH should be a path to a `csh'/`tcsh'-named symlink to the gnash binary,
# or the gnash binary itself (which is then run with --personality=csh).
set -u

gnash=${1:?usage: run_diff_csh.sh GNASH TCSH}
tcsh=${2:-/bin/tcsh}

tmp=$(mktemp /tmp/gnash_csh.XXXXXX)
trap 'rm -f "$tmp"' EXIT

# Run with a hard wall-clock cap so a runaway script (e.g. tcsh looping on a
# malformed loop) can't hang the whole suite.
cap() { perl -e 'alarm 8; exec @ARGV' "$@"; }

# Run the csh script in $tmp through gnash's csh persona.  stdin is closed so a
# script that happens not to read it can't leave the shell waiting on the tty.
run_gnash() {
  case $(basename "$gnash") in
    csh|tcsh) cap "$gnash" -f "$tmp" </dev/null ;;
    *)        cap "$gnash" --personality=csh -f "$tmp" </dev/null ;;
  esac
}

scripts=(
  'echo hello world'
  'echo -n no-newline; echo x'
  'set x = (a b c); echo $#x $x[2] $x[1-3]'
  'set y = ""; echo "[$y]"'
  '@ n = 3 + 4; echo $n'
  '@ n = (2 + 3) * 4; echo $n'
  '@ n = 17 % 5; echo $n'
  '@ n = 10 / 3; echo $n'
  '@ a = 5; @ a += 3; @ a++; echo $a'
  'if (1 == 1) echo yes'
  'if (2 > 5) echo no'
  'if (1 && 1) echo and'
  'if (0 || 1) echo or'
  'set n = 7; if ($n > 5) then'$'\n''echo big'$'\n''else'$'\n''echo small'$'\n''endif'
  'foreach i (1 2 3)'$'\n''echo loop $i'$'\n''end'
  'set k = 0'$'\n''while ($k < 3)'$'\n''echo w$k'$'\n''@ k++'$'\n''end'
  'switch (hello)'$'\n''case hel*:'$'\n''echo matched'$'\n''breaksw'$'\n''default:'$'\n''echo def'$'\n''endsw'
  'setenv GREET hi; echo $GREET'
  'set s = world; echo "quoted $s"'
  "echo 'single \$s literal'"
  'set l = (one two three); echo count=$#l first=$l[1] last=$l[3]'
  'set d = `echo backtick words`; echo $d'
  'set name = x; if ($?name) echo set; if ($?missing) echo unset'
  'set p = /usr/local/bin/gnash; echo $p:h $p:t'
  'set f = archive.tar.gz; echo $f:r $f:e'
  'echo one two three | tr a-z A-Z'
  'echo hello | cat | cat'
  'set c = `echo a b c d | wc -w`; echo words=$c'
  'foreach i (1 2 3 4 5)'$'\n''if ($i == 3) continue'$'\n''if ($i == 5) break'$'\n''echo n=$i'$'\n''end'
  'set i = 1'$'\n''while ($i <= 2)'$'\n''set j = 1'$'\n''while ($j <= 2)'$'\n''echo $i-$j; @ j++'$'\n''end'$'\n''@ i++'$'\n''end'
  'if ("abc" =~ a*) echo m; if ("abc" !~ x*) echo nm'
  '@ x = 2 + 3 * 4; echo $x'
  'set nums = (5 3 8); @ s = $nums[1] + $nums[2] + $nums[3]; echo $s'
  'set items = (red green blue)'$'\n''set i = 1'$'\n''foreach c ($items)'$'\n''echo "${i}: $c"; @ i++'$'\n''end'
  'unset novar; if (! $?novar) echo gone'
  'set empty = (); echo "count=$#empty"'
)

fails=0
for s in "${scripts[@]}"; do
  printf '%s\n' "$s" > "$tmp"
  g_out=$(run_gnash 2>/dev/null); g_rc=$?
  t_out=$(cap "$tcsh" -f "$tmp" </dev/null 2>/dev/null); t_rc=$?
  if [ "$g_out" != "$t_out" ] || [ "$g_rc" != "$t_rc" ]; then
    echo "MISMATCH: $s" >&2
    echo "  gnash (rc=$g_rc): $g_out" >&2
    echo "  tcsh  (rc=$t_rc): $t_out" >&2
    fails=$((fails + 1))
  fi
done

if [ $fails -eq 0 ]; then
  echo "run_diff_csh: all ${#scripts[@]} scripts match tcsh"
  exit 0
fi
echo "run_diff_csh: $fails mismatch(es)" >&2
exit 1
