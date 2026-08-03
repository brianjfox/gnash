#!/usr/bin/env bash
# Copyright (c) 2026 Brian J. Fox
# Licensed under GPLv2 with the GPLv2-AI Exception.
#
# bench.sh -- compare gnash vs bash execution speed across a set of workloads.
#
#   scripts/bench.sh [--gnash PATH] [--bash PATH] [--runs N] [--scale F]
#
# Each workload is run `--runs' times under each shell; the BEST (minimum) wall
# time is reported, which is the least perturbed by scheduling noise.  The ratio
# column is gnash/bash: 1.0 is parity, >1 means gnash is that many times slower,
# <1 means faster.  `--scale' multiplies every loop count (default 1).
#
# Timing uses perl's Time::HiRes and includes shell startup, so the "startup"
# row shows per-invocation overhead and the loop rows amortize it.
set -uo pipefail

GNASH=${GNASH:-/opt/homebrew/bin/gnash}
BASH_BIN=${BASH_BIN:-/opt/homebrew/bin/bash}
RUNS=5
SCALE=1
TIMEOUT=60

while [ $# -gt 0 ]; do
  case "$1" in
    --gnash)   GNASH=${2:?}; shift 2 ;;
    --bash)    BASH_BIN=${2:?}; shift 2 ;;
    --runs)    RUNS=${2:?}; shift 2 ;;
    --scale)   SCALE=${2:?}; shift 2 ;;
    --timeout) TIMEOUT=${2:?}; shift 2 ;;
    -h|--help) sed -n '5,18p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
for b in "$GNASH" "$BASH_BIN"; do [ -x "$b" ] || { echo "not executable: $b" >&2; exit 2; }; done
command -v perl >/dev/null || { echo "perl required for timing" >&2; exit 2; }

# Minimum wall-clock (seconds) of RUNS invocations of `bin --norc -c code',
# each under a per-run timeout.  Prints the min, or "timeout"/"error".
timeit() {
  local bin=$1 code=$2 i
  for ((i = 0; i < RUNS; i++)); do
    perl -MTime::HiRes=time -e '
      use POSIX qw(setsid);
      my ($to, @cmd) = @ARGV;
      my $s = Time::HiRes::time;
      my $pid = fork();
      if ($pid == 0) { setsid(); exec @cmd or exit 127; }
      $SIG{ALRM} = sub { kill(-9, $pid); print "T\n"; exit 0 };
      alarm $to; waitpid($pid, 0); alarm 0;
      exit(1) if $? & 127 or ($? >> 8) >= 126;
      printf "%.6f\n", Time::HiRes::time - $s;
    ' "$TIMEOUT" "$bin" --norc -c "$code"
  done | { grep -v '^T$' || true; } | sort -g | head -1
}

NAMES=(); CODES=()
add() { NAMES+=("$1"); CODES+=("$2"); }

s() { echo $(( $1 * SCALE )); }  # scaled loop count

add "startup (: only)"        ":"
add "arith for-loop"          "for ((i=0;i<$(s 300000);i++)); do :; done"
add "while + test + \$(( ))"    "i=0; while [ \$i -lt $(s 200000) ]; do i=\$((i+1)); done"
add "param expansion"         "x=abcdefghij; for ((i=0;i<$(s 100000);i++)); do y=\${x#a}; y=\${x^^}; done"
add "function calls"          "f(){ :; }; for ((i=0;i<$(s 200000);i++)); do f; done"
add "printf builtin"          "for ((i=0;i<$(s 50000);i++)); do printf '%d\n' \$i; done >/dev/null"
add "echo builtin"            "for ((i=0;i<$(s 100000);i++)); do echo hello; done >/dev/null"
add "case / pattern"          "for ((i=0;i<$(s 100000);i++)); do case abc.txt in *.txt) :;; *) :;; esac; done"
add "array build + sum"       "a=(); for ((i=0;i<$(s 50000);i++)); do a[i]=\$i; done; s=0; for v in \"\${a[@]}\"; do s=\$((s+v)); done"
add "string concat"           "s=; for ((i=0;i<$(s 20000);i++)); do s+=x; done"
add "subshell spawn"          "for ((i=0;i<$(s 3000);i++)); do (:); done"
add "fork+exec (/bin/true)"   "for ((i=0;i<$(s 2000);i++)); do /usr/bin/true; done"

printf '%s vs %s   (runs=%d, scale=%s, best-of times in seconds)\n\n' \
  "gnash $($GNASH -c 'echo $GNASH_VERSION' 2>/dev/null)" \
  "bash $($BASH_BIN -c 'echo ${BASH_VERSION%%(*}' 2>/dev/null)" "$RUNS" "$SCALE"
printf '%-28s %10s %10s %8s\n' "workload" "bash" "gnash" "ratio"
printf '%-28s %10s %10s %8s\n' "--------" "----" "-----" "-----"

sum_ratio=0; nr=0
for i in "${!NAMES[@]}"; do
  bt=$(timeit "$BASH_BIN" "${CODES[$i]}")
  gt=$(timeit "$GNASH" "${CODES[$i]}")
  if [[ -z "$bt" || -z "$gt" ]]; then
    printf '%-28s %10s %10s %8s\n' "${NAMES[$i]}" "${bt:-err}" "${gt:-err}" "-"
    continue
  fi
  ratio=$(perl -e 'printf "%.2f", $ARGV[1]/$ARGV[0]' "$bt" "$gt")
  printf '%-28s %10.4f %10.4f %7sx\n' "${NAMES[$i]}" "$bt" "$gt" "$ratio"
  sum_ratio=$(perl -e 'print $ARGV[0]+$ARGV[1]' "$sum_ratio" "$ratio"); nr=$((nr+1))
done

[ "$nr" -gt 0 ] && printf '\nmean ratio (gnash/bash): %.2fx over %d workloads\n' \
  "$(perl -e 'print $ARGV[0]/$ARGV[1]' "$sum_ratio" "$nr")" "$nr"
