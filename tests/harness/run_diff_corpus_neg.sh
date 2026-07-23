#!/usr/bin/env bash
# Copyright (c) 2026 Brian J. Fox
# Licensed under GPLv2 with the GPLv2-AI Exception.
#
# run_diff_corpus_neg.sh GNASH_PARSE BASH [NEG_CORPUS_NUL]
#
# NEGATIVE parser differential.  Reads a NUL-delimited corpus of deliberately
# malformed snippets (default: gen_neg_corpus.sh) and, for each, compares the
# accept/reject verdict of `gnash-parse' against `bash -n' (exit 0 = accept).
#
# Where run_diff_corpus_parse.sh proves gnash accepts what bash accepts, this
# proves gnash REJECTS what bash rejects -- the leniency half a valid-only corpus
# cannot measure.  The headline metric is: of the snippets bash rejects, how many
# does gnash also reject?  A `gnash accepts / bash rejects' case is a real
# leniency bug.  No snippet is executed; parsing only, so it is entirely safe.
set -u

gp=${1:?usage: run_diff_corpus_neg.sh GNASH_PARSE BASH [NEG_CORPUS_NUL]}
bash_bin=${2:?usage: run_diff_corpus_neg.sh GNASH_PARSE BASH [NEG_CORPUS_NUL]}
corpus=${3:-}

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

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
if [ -z "$corpus" ]; then
  corpus=$(mktemp)
  "$bash_bin" "$here/gen_neg_corpus.sh" > "$corpus"
fi

work=$(mktemp -d)
snipf="$work/snippet.sh"
trap 'rm -rf "$work"' EXIT

report=${GNASH_REPORT:-run_diff_corpus_neg.report}
: > "$report"

total=0 both_reject=0 both_accept=0 leniency=0 overstrict=0
bash_rejects=0 gnash_rejects_of_bash=0
while IFS= read -r -d '' snip; do
  [ -n "$snip" ] || continue
  total=$((total + 1))
  printf '%s\n' "$snip" > "$snipf"
  "$gp" "$snipf" >/dev/null 2>&1;        g_acc=$([ $? -eq 0 ] && echo 1 || echo 0)
  "$bash_bin" -n "$snipf" >/dev/null 2>&1; b_acc=$([ $? -eq 0 ] && echo 1 || echo 0)

  [ "$b_acc" = 0 ] && bash_rejects=$((bash_rejects + 1))
  [ "$b_acc" = 0 ] && [ "$g_acc" = 0 ] && gnash_rejects_of_bash=$((gnash_rejects_of_bash + 1))

  if   [ "$g_acc" = 0 ] && [ "$b_acc" = 0 ]; then both_reject=$((both_reject + 1))
  elif [ "$g_acc" = 1 ] && [ "$b_acc" = 1 ]; then
    both_accept=$((both_accept + 1))
    printf 'BOTH_ACCEPT (not actually invalid)   %s\n' "$snip" >> "$report"
  elif [ "$g_acc" = 1 ] && [ "$b_acc" = 0 ]; then
    leniency=$((leniency + 1))
    printf 'LENIENCY (gnash accepts, bash rejects)  %s\n' "$snip" >> "$report"
  else
    overstrict=$((overstrict + 1))
    printf 'OVERSTRICT (gnash rejects, bash accepts) %s\n' "$snip" >> "$report"
  fi
done < "$corpus"

lpct="n/a"
[ "$bash_rejects" -gt 0 ] && \
  lpct=$(awk "BEGIN{printf \"%.1f\", 100*$gnash_rejects_of_bash/$bash_rejects}")

echo "=== negative parser differential ==="
echo "malformed snippets       : $total"
echo "both reject (agree)      : $both_reject"
echo "both accept (not invalid): $both_accept"
echo "LENIENCY  gnash acc/bash rej : $leniency   <- real gaps if > 0"
echo "overstrict gnash rej/bash acc: $overstrict"
echo "bash rejected            : $bash_rejects"
echo "REJECTION AGREEMENT      : $gnash_rejects_of_bash/$bash_rejects = ${lpct}%"
echo "divergences -> $report"
[ "$leniency" -eq 0 ]
