# Benchmarking gnash against bash

gnash aims to be a drop-in replacement for bash, and "drop-in" includes
performance: build systems, `configure` scripts, and CI pipelines fork and
loop millions of times, so a shell that is several times slower is a real
regression even when it is byte-for-byte correct. This document describes the
benchmark harness (`scripts/bench.sh`), how to run it, and the results on the
reference machine.

## Running the benchmark

```sh
scripts/bench.sh                       # gnash and bash from /opt/homebrew/bin
scripts/bench.sh --gnash ./build/core/gnash --bash /opt/homebrew/bin/bash
scripts/bench.sh --runs 9 --scale 2    # more repetitions, larger loops
```

Each workload is a small shell program run with `SHELL --norc -c '...'`. It is
executed `--runs` times (default 5) under each shell, and the **best**
(minimum) wall-clock time is kept — the minimum is the run least perturbed by
scheduler noise and other processes. Timing uses perl's `Time::HiRes` and wraps
the entire invocation, so shell **startup is included** in every measurement:
the `startup` row shows the per-invocation overhead, and the loop rows amortize
it over their iterations.

The reported `ratio` is `gnash / bash`: **1.0 is parity, below 1.0 means gnash
is faster, above 1.0 means gnash is slower** by that factor.

### Methodology notes

- **Compare optimized builds.** Use a release/optimized gnash (the Homebrew
  bottle, or a `-DCMAKE_BUILD_TYPE=Release` build) against an optimized bash.
  A debug build of either shell makes the comparison meaningless.
- **Same bash family.** Results below use bash 5.3, the version gnash targets
  for conformance, so behavior — and therefore the work each shell does — is
  the same.
- **Microbenchmarks are directional.** These isolate one construct at a time.
  Absolute numbers are machine-specific; the ratios are what matter, and even
  those shift with CPU, libc, and filesystem. Treat them as a guide, not a
  contract.

## Workloads

| Workload | What it stresses |
|---|---|
| `startup (: only)` | fork + exec + shell initialization, per invocation |
| `arith for-loop` | `for (( ))` loop and arithmetic evaluation |
| `while + test + $(( ))` | `while`, the `[ ]` test command, arithmetic substitution |
| `param expansion` | `${x#…}`, `${x^^}` and friends in a hot loop |
| `function calls` | shell-function call/return overhead |
| `printf builtin` | the `printf` builtin and format parsing |
| `echo builtin` | the `echo` builtin and output |
| `case / pattern` | `case` with a glob pattern per iteration |
| `array build + sum` | indexed-array element writes and iteration |
| `string concat` | repeated `+=` string growth |
| `subshell spawn` | `( … )` subshell fork + teardown |
| `fork+exec (/bin/true)` | launching an external command |

## Results

Reference machine: Apple Silicon, macOS (Tahoe). gnash built from this tree,
bash 5.3.15 from Homebrew. Best of 5 runs; times in seconds.

| Workload | bash | gnash | ratio (gnash/bash) |
|---|---:|---:|---:|
| startup (: only) | 0.0037 | 0.0020 | **0.55×** |
| arith for-loop | 0.2426 | 0.2117 | **0.87×** |
| while + test + `$(( ))` | 0.2882 | 0.3916 | 1.36× |
| param expansion | 0.1728 | 0.1545 | **0.89×** |
| function calls | 0.3186 | 0.3541 | 1.11× |
| printf builtin | 0.0863 | 0.1018 | 1.18× |
| echo builtin | 0.1476 | 0.1524 | 1.03× |
| case / pattern | 0.1209 | 0.0968 | **0.80×** |
| array build + sum | 0.1052 | 0.0914 | **0.87×** |
| string concat | 0.0219 | 0.0275 | 1.26× |
| subshell spawn | 1.0118 | 0.8649 | **0.85×** |
| fork+exec (/bin/true) | 2.0715 | 1.9653 | **0.95×** |
| **mean ratio** | | | **0.98×** |

## Interpretation

Overall gnash is **at parity with bash** on this suite (mean 0.98×), and faster
on several common paths:

- **Startup is ~2× faster** (0.55×). Short-lived shell invocations — the bread
  and butter of `configure` scripts and Makefile recipes — start quicker.
- **Subshells and external commands** (0.85× / 0.95×) are as fast or faster,
  which matters most for fork-heavy workloads.
- **Arithmetic loops, parameter expansion, arrays, and `case`/pattern matching**
  are all modestly faster (0.80×–0.89×).
- **Builtin-heavy loops and function calls** are slightly slower (`printf`
  1.18×, `string concat` 1.26×, `while + test` 1.36×, `function calls` 1.11×) —
  within a small constant factor, not an order of magnitude.

There is no performance cliff: no workload is dramatically slower, and the
common short-lived and fork-heavy cases favor gnash.

## A hotspot the benchmark caught

The first run of this suite showed `while [ … ]` loops at roughly **13× bash**,
against a mean of ~2×. Decomposing it revealed the cause was not `while`,
arithmetic, or the test operator itself — `while (( … ))` and `while test …`
were both fine — but the `[ ]` **bracket** form specifically.

The root cause: gnash flagged any word containing an unquoted `[` as a glob
pattern, so the `[ … ]` command did an `opendir`/`readdir` of the current
directory on *every* invocation. Bash treats a `[` that has no matching `]` as
an ordinary character and skips the scan. gnash now does the same — a `[` is a
glob metacharacter only when a closing `]` follows — which removed the scan,
dropped `[ x = x ]` from ~13× to ~1.4×, and (as a bonus) fixed a handful of
`glob` conformance cases where a bare `[` should stay literal.

This is the benchmark's real job: not to publish a single number, but to catch
the outliers where gnash does asymptotically more work than bash, so they can be
fixed.
