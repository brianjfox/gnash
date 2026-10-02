#!/usr/bin/env bash
# Copyright (c) 2026 Brian J. Fox
# Licensed under GPLv2 with the GPLv2-AI Exception.

# invocation_test.sh GNASH BASH
#
# Differential check of *argv handling at invocation* vs bash: which words are
# options, which word is the -c command string, and which become $0 and the
# positional parameters.  run_diff.sh always invokes `-c SCRIPT', so it cannot
# see any of this.  Exit 0 if all forms agree.
set -u

gnash=${1:?usage: invocation_test.sh GNASH BASH}
bash=${2:?usage: invocation_test.sh GNASH BASH}

# The oracle must be a REAL bash; a gnash symlink often shadows `bash' on a dev
# box, and a differential against gnash itself is meaningless.  (Same probe as
# run_diff.sh.)
is_real_bash() { "$1" --version 2>/dev/null | head -1 | grep -qi 'GNU bash'; }
if ! is_real_bash "$bash"; then
  set +u  # GNASH_BASH may be unset
  found=""
  for cand in "$GNASH_BASH" /opt/homebrew/Cellar/bash/*/bin/bash \
              /usr/local/bin/bash /opt/local/bin/bash /bin/bash; do
    if [ -n "$cand" ] && [ -x "$cand" ] && is_real_bash "$cand"; then found=$cand; break; fi
  done
  set -u
  if [ -z "$found" ]; then
    echo "invocation_test: no real bash oracle found (got '$bash'); skipping" >&2
    exit 0
  fi
  bash=$found
fi

fails=0

# check ARGV...  -- run the argv under both shells, compare stdout and status.
# stderr is dropped: the diagnostics name the shell differently (errfmt.sh
# covers message format).
check() {
  local g_out b_out g_rc b_rc
  g_out=$("$gnash" "$@" </dev/null 2>/dev/null); g_rc=$?
  b_out=$("$bash"  "$@" </dev/null 2>/dev/null); b_rc=$?
  if [ "$g_out" != "$b_out" ] || [ "$g_rc" != "$b_rc" ]; then
    echo "MISMATCH: $*" >&2
    echo "  gnash (rc=$g_rc): $g_out" >&2
    echo "  bash  (rc=$b_rc): $b_out" >&2
    fails=$((fails + 1))
  fi
  count=$((count + 1))
}

count=0

# Options before -c, and grouped in the same word.
check -c 'echo ok'
check -lc 'echo ok'
check -l -c 'echo ok'
check --login -c 'echo ok'
check -ce 'echo ok'

# Options AFTER -c: the command string is the first non-option word, not the
# word that follows -c (#704).
check -c -l 'echo ok'
check -c -e 'echo ok'
check -c -l -e 'echo ok'
check -c -o posix 'echo $-'
check -c -O expand_aliases 'shopt -q expand_aliases && echo on'
check -c -- 'echo ok'

# $0 and the positional parameters come after the command string, whichever
# side of -c the options sat on.
check -c 'echo "[$0][$1][$2][$#]"' name a b
check -c -l 'echo "[$0][$1][$2][$#]"' name a b
check -c 'echo "[$0][$*]"' -l -e

# -c with no command word at all.
check -c
check -c -l

if [ $fails -eq 0 ]; then
  echo "invocation_test: all $count invocations match bash"
  exit 0
fi
echo "invocation_test: $fails mismatch(es)" >&2
exit 1
