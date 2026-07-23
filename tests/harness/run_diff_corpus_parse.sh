#!/usr/bin/env bash
# Copyright (c) 2026 Brian J. Fox
# Licensed under GPLv2 with the GPLv2-AI Exception.
#
# run_diff_corpus_parse.sh GNASH_PARSE BASH [CORPUS_LIST]
#
# Corpus-scale PARSER differential.  For every script in the corpus, run
# `gnash-parse` and `bash -n` and compare the accept/reject verdict (exit 0 =
# accept, non-zero = reject/syntax-error).  This is a *syntax* differential: it
# never executes the scripts, so it is safe to point at thousands of real,
# untrusted scripts harvested from the system.
#
# CORPUS_LIST is a file with one script path per line.  If omitted, a corpus is
# harvested from the usual system locations (Homebrew, /usr/bin, /usr/share, the
# bash source tree) by extension and shebang.
#
# Output: a 2x2 agreement matrix and an agreement percentage, plus a report of
# every divergence written to run_diff_corpus_parse.report.
set -u

gp=${1:?usage: run_diff_corpus_parse.sh GNASH_PARSE BASH [CORPUS_LIST]}
bash_bin=${2:?usage: run_diff_corpus_parse.sh GNASH_PARSE BASH [CORPUS_LIST]}
corpus_list=${3:-}

is_real_bash() { "$1" --version 2>/dev/null | head -1 | grep -qi 'GNU bash'; }
if ! is_real_bash "$bash_bin"; then
  found=""
  for cand in "${GNASH_BASH:-}" /opt/homebrew/Cellar/bash/*/bin/bash \
              /usr/local/bin/bash /opt/local/bin/bash /bin/bash; do
    if [ -n "$cand" ] && [ -x "$cand" ] && is_real_bash "$cand"; then found=$cand; break; fi
  done
  [ -z "$found" ] && { echo "no real bash oracle found; skipping" >&2; exit 0; }
  bash_bin=$found
fi

# Portable bounded run: kill the child if it exceeds N seconds (parsing should
# be near-instant; a hang means a pathological input).  Returns the child rc, or
# 124 on timeout.
run_bounded() {
  local secs=$1; shift
  "$@" >/dev/null 2>&1 &
  local pid=$!
  ( sleep "$secs"; kill -9 "$pid" 2>/dev/null ) 2>/dev/null &
  local watcher=$!
  wait "$pid" 2>/dev/null; local rc=$?
  kill "$watcher" 2>/dev/null; wait "$watcher" 2>/dev/null
  return $rc
}

# Build the corpus if none was supplied.
if [ -z "$corpus_list" ]; then
  corpus_list=$(mktemp)
  for r in /opt/homebrew /usr/bin /bin /usr/share /usr/libexec \
           /Users/bfox/CLIENTS/BJF/BASH/bash; do
    find "$r" -type f \( -name '*.sh' -o -name '*.bash' -o -name 'configure' \
         -o -name '*.bats' \) 2>/dev/null || true
  done | sort -u > "$corpus_list"
fi

report=${GNASH_REPORT:-run_diff_corpus_parse.report}
: > "$report"

total=0 both_accept=0 both_reject=0 g_only_accept=0 b_only_accept=0
while IFS= read -r f; do
  [ -f "$f" ] && [ -r "$f" ] || continue
  total=$((total + 1))
  run_bounded 10 "$gp" "$f";        g_rc=$?
  run_bounded 10 "$bash_bin" -n "$f"; b_rc=$?
  g_acc=0; [ "$g_rc" -eq 0 ] && g_acc=1
  b_acc=0; [ "$b_rc" -eq 0 ] && b_acc=1
  if   [ $g_acc -eq 1 ] && [ $b_acc -eq 1 ]; then both_accept=$((both_accept+1))
  elif [ $g_acc -eq 0 ] && [ $b_acc -eq 0 ]; then both_reject=$((both_reject+1))
  elif [ $g_acc -eq 1 ] && [ $b_acc -eq 0 ]; then
    g_only_accept=$((g_only_accept+1))
    echo "GNASH_ACCEPTS_BASH_REJECTS  (g=$g_rc b=$b_rc)  $f" >> "$report"
  else
    b_only_accept=$((b_only_accept+1))
    echo "GNASH_REJECTS_BASH_ACCEPTS  (g=$g_rc b=$b_rc)  $f" >> "$report"
  fi
done < "$corpus_list"

agree=$((both_accept + both_reject))
pct="n/a"
[ "$total" -gt 0 ] && pct=$(awk "BEGIN{printf \"%.1f\", 100*$agree/$total}")

echo "=== corpus parser differential ==="
echo "corpus scripts        : $total"
echo "both accept           : $both_accept"
echo "both reject           : $both_reject"
echo "gnash accepts/bash rej: $g_only_accept   (gnash too lenient)"
echo "gnash rejects/bash acc: $b_only_accept   (gnash too strict / parser gap)"
echo "AGREEMENT             : $agree/$total = ${pct}%"
echo "divergences -> $report"
