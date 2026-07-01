#!/usr/bin/env bash
# conformance.sh GNASH [testdir]
#
# Run each of bash's own tests/*.tests under gnash and diff the combined
# stdout+stderr against the matching *.right file (the same scoring bash's
# run-all uses).  Prints a PASS/FAIL summary and the list of passing tests.
#
# This is a progress metric, not a hard gate: gnash's error-message text differs
# from bash's, so tests whose .right captures shell error strings will diff even
# when behaviour is correct.
set -u

gnash=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
dir=${2:-$(dirname "$0")/../bash-tests}
dir=$(cd "$dir" && pwd)

export THIS_SH="$gnash"
export BASH_TSTOUT=/tmp/gnash_tst_$$
PATH="$dir:$PATH"
export PATH

pass=0
fail=0
passing=""

# Per-test timeout that kills the whole process group (children included),
# since `timeout' may be absent and tests spawn sub-shells.
run_timed() {
  local secs=$1; shift
  perl -e '
    use POSIX qw(setsid);
    my $secs = shift;
    my $pid = fork();
    if ($pid == 0) { setsid(); exec @ARGV or exit 127; }
    $SIG{ALRM} = sub { kill(-9, $pid); exit 124; };
    alarm $secs; waitpid($pid, 0); alarm 0;
  ' "$secs" "$@"
}

for t in "$dir"/*.tests; do
  base=$(basename "$t" .tests)
  right="$dir/$base.right"
  [ -f "$right" ] || continue
  # Write to a temp file (not command substitution) so a lingering grandchild
  # holding stdout open can't block us.
  tmp="$BASH_TSTOUT.$base"
  ( cd "$dir" && run_timed 5 "$gnash" "./$base.tests" </dev/null >"$tmp" 2>&1 )
  if diff -q "$right" "$tmp" >/dev/null 2>&1; then
    pass=$((pass + 1))
    passing="$passing $base"
    echo "PASS $base"
  else
    fail=$((fail + 1))
    echo "fail $base"
  fi
  rm -f "$tmp"
done

rm -f "$BASH_TSTOUT"
total=$((pass + fail))
echo "=== gnash conformance: $pass/$total bash tests pass ==="
echo "passing:$passing"
