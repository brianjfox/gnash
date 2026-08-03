# gnash 1.8.0

A performance release. gnash now runs at parity with bash across a suite of
common workloads — and is faster on several — after removing a pathological
slowdown in the `[ ]` test command. This release also adds a benchmark harness
and documents the methodology and results.

## Performance

- **`[ ... ]` no longer scans the filesystem.** A word containing an unquoted
  `[` was always treated as a glob pattern, so the `[ ... ]` test command did an
  `opendir`/`readdir` of the current directory on every call. `while [ ... ]`
  loops were therefore roughly 13× slower than bash (and than the equivalent
  `test` or `(( ))`). A `[` is now a glob metacharacter only when a matching
  `]` follows, exactly as bash decides — a bare `[` is an ordinary character
  and skips the scan. `[ x = x ]` in a tight loop drops from ~13× to ~1.4×, and
  a few `glob` conformance cases where a bare `[` should stay literal are fixed
  as a bonus.
- With that outlier gone, gnash is at parity with bash overall (mean 0.98× on
  the benchmark suite): faster on startup (~2×), subshell spawn, arithmetic
  loops, arrays, and `case`/pattern matching; within a small factor on
  builtin-heavy loops and function calls; no workload is dramatically slower.

## Tooling and documentation

- `scripts/bench.sh` times a set of workloads (startup, loops, expansion,
  builtins, function calls, subshell/fork) under gnash and bash and reports the
  gnash/bash ratio, using a best-of-N wall-clock timer.
- `BENCHMARK.md` documents the harness, methodology, per-workload results, and
  the hotspot that the benchmark itself surfaced.
