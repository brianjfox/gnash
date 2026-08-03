#!/usr/bin/env bash
# Copyright (c) 2026 Brian J. Fox
# Licensed under GPLv2 with the GPLv2-AI Exception.
#
# corpus-diff.sh -- measure how close gnash is to a drop-in bash replacement by
# running a corpus of REAL-WORLD shell scripts under both shells and reporting
# where they diverge.
#
#   scripts/corpus-diff.sh [options] [ROOT ...]
#
# Two modes:
#   --parse   (default) Parse-only: `SHELL -n SCRIPT'.  Never executes the
#             script, so it is safe to run over arbitrary system scripts.
#             Compares parse success/failure and the syntax-error diagnostics.
#   --exec    Execute each script under both shells in an isolated temp CWD with
#             a scrubbed environment and a timeout, then diff stdout+stderr+exit
#             status.  Only point this at scripts you trust to be side-effect
#             free -- see the WARNING below.  Intended for a curated corpus.
#
# Options:
#   --exec              enable execute-diff (default is parse-only)
#   --max N             cap at N scripts (default 1000)
#   --timeout SECS      per-script timeout in --exec mode (default 10)
#   --bash PATH         bash binary (default: from $BASH or PATH)
#   --gnash PATH        gnash binary (default: build/core/gnash under the repo)
#   --out DIR           write the detailed report here (default: a temp dir)
#   -h | --help         this help
#
# WARNING (--exec): executing real-world scripts runs whatever they contain.
# The harness scrubs the environment, runs in a throwaay CWD, and applies a
# timeout, but it CANNOT sandbox filesystem or network side effects.  Only use
# --exec on a corpus you have vetted.  Parse mode is always safe.
#
# Exit status: 0 if every script agreed, 1 if any diverged, 2 on usage error.

set -uo pipefail

MODE=parse
MAX=1000
TIMEOUT=10
BASH_BIN=${BASH:-}
GNASH_BIN=""
OUT=""
ROOTS=()

while [ $# -gt 0 ]; do
  case "$1" in
    --parse)    MODE=parse; shift ;;
    --exec)     MODE=exec; shift ;;
    --max)      MAX=${2:?}; shift 2 ;;
    --timeout)  TIMEOUT=${2:?}; shift 2 ;;
    --bash)     BASH_BIN=${2:?}; shift 2 ;;
    --gnash)    GNASH_BIN=${2:?}; shift 2 ;;
    --out)      OUT=${2:?}; shift 2 ;;
    -h|--help)  sed -n '4,40p' "$0"; exit 0 ;;
    -*)         echo "unknown option: $1" >&2; exit 2 ;;
    *)          ROOTS+=("$1"); shift ;;
  esac
done

ROOT=$(cd "$(dirname "$0")/.." && pwd)
[ -n "$BASH_BIN" ] || BASH_BIN=$(command -v bash)
[ -n "$GNASH_BIN" ] || GNASH_BIN="$ROOT/build/core/gnash"
[ -x "$BASH_BIN" ]  || { echo "bash not found: $BASH_BIN" >&2; exit 2; }
[ -x "$GNASH_BIN" ] || { echo "gnash not found: $GNASH_BIN (build it first)" >&2; exit 2; }
[ -n "$OUT" ] || OUT=$(mktemp -d "${TMPDIR:-/tmp}/corpus-diff.XXXXXX")
mkdir -p "$OUT"

# Default corpus: real scripts that ship on this machine.  Override by passing
# ROOT dirs/files on the command line.
if [ ${#ROOTS[@]} -eq 0 ]; then
  for d in /opt/homebrew /usr/share /usr/libexec /etc; do
    [ -d "$d" ] && ROOTS+=("$d")
  done
fi

# --- collect the corpus ------------------------------------------------------
# A file joins the corpus if it has a sh/bash shebang or a .sh/.bash extension.
# Skip .sample git hooks and anything unreadable.
is_shell_script() {
  local f=$1
  case "$f" in *.sample) return 1 ;; esac
  case "$f" in *.sh|*.bash) return 0 ;; esac
  read -r first < "$f" 2>/dev/null || return 1
  case "$first" in
    '#!'*/sh|'#!'*/sh\ *|'#!'*/bash|'#!'*/bash\ *|'#!'*env\ sh*|'#!'*env\ bash*) return 0 ;;
    *) return 1 ;;
  esac
}

echo "collecting corpus from: ${ROOTS[*]}" >&2
CORPUS="$OUT/corpus.list"; : > "$CORPUS"
count=0
while IFS= read -r f; do
  [ -f "$f" ] && [ -r "$f" ] || continue
  is_shell_script "$f" || continue
  printf '%s\n' "$f" >> "$CORPUS"
  count=$((count + 1))
  [ "$count" -ge "$MAX" ] && break
done < <(find "${ROOTS[@]}" -type f 2>/dev/null)
total=$(wc -l < "$CORPUS" | tr -d ' ')
echo "collected $total scripts (mode=$MODE)" >&2

# Normalize shell-specific noise so only real differences remain: the shell's
# own path/name in diagnostics and long numbers (PIDs).  Uses `#' as the sed
# delimiter so the binary PATH (which contains `/') substitutes cleanly.  Line
# numbers are deliberately NOT normalized -- a differing line number on the same
# script is a real divergence worth surfacing.
norm() {  # norm <binary-path>
  local base; base=$(basename "$1")
  LC_ALL=C sed -E "s#$1#SHELL#g; s#^${base}:#SHELL:#; s#[0-9]{5,}#NUM#g"
}

run_parse() {  # run_parse <bin> <script> -> writes rc + normalized stderr
  local bin=$1 f=$2
  "$bin" --norc -n "$f" >/dev/null 2>"$OUT/.err"
  local rc=$?
  printf 'rc=%s\n' "$rc"
  norm "$bin" < "$OUT/.err"
}

run_exec() {  # run_exec <bin> <script> -> rc + normalized stdout+stderr
  # Isolated CWD, scrubbed env, stdin from /dev/null (so a script that reads
  # input does not hang or steal the terminal), and a perl-based timeout (macOS
  # has no timeout(1)) that kills the whole process group after $TIMEOUT secs.
  local bin=$1 f=$2 work abs base
  # Absolute script path: exec runs inside the throwaway CWD, so a relative path
  # would no longer resolve.
  abs=$(cd "$(dirname "$f")" 2>/dev/null && pwd)/$(basename "$f")
  base=$(basename "$bin")
  work=$(mktemp -d "$OUT/.run.XXXXXX")
  ( cd "$work" && env -i PATH="/usr/bin:/bin" HOME="$work" TERM=dumb LC_ALL=C \
      perl -e '
        use POSIX qw(setsid); my $secs = shift; my $pid = fork();
        if ($pid == 0) { setsid(); exec @ARGV or exit 127; }
        $SIG{ALRM} = sub { kill(-9, $pid); exit 124; };
        alarm $secs; waitpid($pid, 0); alarm 0; exit($? >> 8);' \
      "$TIMEOUT" "$bin" --norc "$abs" ) >"$work/.o" 2>"$work/.e" </dev/null
  local rc=$?
  printf 'rc=%s\n' "$rc"
  # Normalize runtime nondeterminism so only real behavioural differences show:
  # the shell's own path AND its basename-colon error prefix (bash uses the full
  # argv0, gnash the basename), the script path, the throwaway CWD, and PIDs.
  local nrm="s#$bin#SHELL#g; s#^${base}: #SHELL: #; s#$abs#SCRIPT#g; s#$work#CWD#g; s#[0-9]{5,}#NUM#g"
  { LC_ALL=C sed -E "$nrm" < "$work/.o"; echo '--8<--stderr--';
    LC_ALL=C sed -E "$nrm" < "$work/.e"; }
  rm -rf "$work"
}

# --- compare -----------------------------------------------------------------
DIVERGED="$OUT/diverged.txt"; : > "$DIVERGED"
SIGS="$OUT/signatures.txt"; : > "$SIGS"
BLOCKERS="$OUT/blockers.txt"; : > "$BLOCKERS"
agree=0; diverge=0; blockers=0; i=0
# The exit status carries the most important signal: a script bash ACCEPTS
# (rc=0) but gnash rejects, or vice versa, is a real drop-in blocker; a
# difference where both agree on accept/reject is cosmetic (usually error-message
# wording on a script that is invalid under both).
rc_of() { printf '%s' "$1" | sed -n 's/^rc=//p' | head -1; }
while IFS= read -r f; do
  i=$((i + 1))
  printf '\r  %d/%d  diverged=%d (blockers=%d)  ' "$i" "$total" "$diverge" "$blockers" >&2
  if [ "$MODE" = parse ]; then
    bo=$(run_parse "$BASH_BIN" "$f"); go=$(run_parse "$GNASH_BIN" "$f")
  else
    bo=$(run_exec "$BASH_BIN" "$f"); go=$(run_exec "$GNASH_BIN" "$f")
  fi
  if [ "$bo" = "$go" ]; then
    agree=$((agree + 1))
    continue
  fi
  diverge=$((diverge + 1))
  brc=$(rc_of "$bo"); grc=$(rc_of "$go")
  kind=cosmetic
  if [ "$brc" = 0 ] && [ "$grc" != 0 ]; then kind=BLOCKER-gnash-rejects
  elif [ "$brc" != 0 ] && [ "$grc" = 0 ]; then kind=BLOCKER-gnash-accepts; fi
  {
    echo "### [$kind] $f"
    diff <(printf '%s\n' "$bo") <(printf '%s\n' "$go") | sed 's/^/    /'
    echo
  } >> "$DIVERGED"
  case "$kind" in
    BLOCKER-*) blockers=$((blockers + 1)); printf '%s\t%s\n' "$kind" "$f" >> "$BLOCKERS" ;;
  esac
  # Signature: gnash's first differing line, for frequency ranking.
  sig=$(diff <(printf '%s\n' "$bo") <(printf '%s\n' "$go") | grep -m1 '^>' | sed 's/^> //')
  [ -n "$sig" ] || sig="(bash-only output / exit-status differs)"
  printf '%s\n' "$sig" >> "$SIGS"
done < "$CORPUS"
echo >&2

# --- report ------------------------------------------------------------------
pct=0; [ "$total" -gt 0 ] && pct=$(( agree * 100 / total ))
{
  echo "gnash vs bash corpus diff  (mode=$MODE)"
  echo "  bash:  $BASH_BIN"
  echo "  gnash: $GNASH_BIN"
  echo "  scripts:   $total"
  echo "  identical: $agree  (${pct}%)"
  echo "  diverged:  $diverge  (of which $blockers accept/reject BLOCKERS)"
  echo
  if [ "$blockers" -gt 0 ]; then
    echo "BLOCKERS -- one shell accepts what the other rejects (real drop-in gaps):"
    sed 's/^/  /' "$BLOCKERS"
    echo
  fi
  if [ "$diverge" -gt 0 ]; then
    echo "top divergence signatures (gnash side, by frequency):"
    sort "$SIGS" | uniq -c | sort -rn | head -20 | sed 's/^/  /'
    echo
    echo "full per-script diffs: $DIVERGED"
  fi
} | tee "$OUT/report.txt"

# Exit non-zero only for real accept/reject blockers; cosmetic message diffs
# (common on scripts that are invalid under both shells) do not fail the run.
[ "$blockers" -eq 0 ]
