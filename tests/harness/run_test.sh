#!/usr/bin/env bash
# run_test.sh SHELL TESTFILE RIGHTFILE
#
# Run TESTFILE under SHELL and compare the combined stdout+stderr against
# RIGHTFILE, exactly as bash's own tests/ suite is scored.  Exit 0 on a match,
# non-zero (printing the diff) otherwise.
#
# This is the conformance oracle that later phases point at the gnash binary.
set -u

if [ "$#" -ne 3 ]; then
  echo "usage: run_test.sh SHELL TESTFILE RIGHTFILE" >&2
  exit 2
fi

shell=$1
testfile=$2
rightfile=$3

actual=$("$shell" "$testfile" 2>&1)
status=$?

if diff -u "$rightfile" <(printf '%s\n' "$actual") ; then
  exit 0
else
  echo "--- test $testfile FAILED (shell exit $status) ---" >&2
  exit 1
fi
