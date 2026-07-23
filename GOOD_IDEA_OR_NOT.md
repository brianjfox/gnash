# gnash: Good Idea or Not?

An evaluation of the gnash refactor — a modular C++ reimplementation of GNU Bash —
scored on a scale of 1–100.

## What is actually being evaluated

`gnash` is  a
~27,000-line modular C++ reimplementation of bash's ~86,000-line C core, aiming
for byte-identical behavior across five shell personalities (bash, ash/dash, ksh,
zsh, csh/tcsh), largely AI-authored (~16 hours of combined human + compute effort
per the README). So the question being scored is: **is this reimplementation-of-bash
worth pursuing?**

The honest answer depends heavily on what it is measured against, so the axis is
made explicit below.

## Measured scope (as of this evaluation)

| Metric | Value |
|---|---|
| gnash source (excl. build) | ~27K lines across 81 files |
| Upstream bash top-level C | ~86K lines |
| Largest modules | `core/` 17.3K, `libreadline` 3.5K, `libhistory` 1.9K, `libglob` 741 |
| Differential exec harness | 176 scripts, identical stdout + exit vs bash 5.3 |
| Parser oracle | parses all 83 bash test files, 74/83 accept/reject agreement with `bash -n` |
| Persona harnesses | csh vs tcsh 6.21 (36), zsh (43), ksh vs ksh93 (37) |

## Pros (grounded in the actual tree)

- **The architecture genuinely beats bash's.** Upstream bash is a monolith of
  global state and macro soup. gnash carves out real standalone libraries with API
  seams — `libglob`, `libhistory`, `libreadline`, `libtermcap` are independently
  usable. That modularity is a legitimate structural improvement, not cosmetic.
- **Memory safety by construction.** RAII, `std` containers, `gnash::sh` checked
  allocation. Bash's history is littered with buffer/parser CVEs (Shellshock being
  the famous one). A typed, bounds-checked rewrite eliminates whole bug *classes*.
- **Density.** ~27K lines reaching substantial 5.3 compatibility vs. ~86K. Less
  surface to maintain per unit of behavior.
- **Differential testing is baked in, not bolted on** — `run_diff.sh` (176 scripts),
  the `bash -n` parser oracle, and per-persona harnesses vs. tcsh/zsh/ksh. That is
  the right discipline for a compatibility project.
- **Personalities are real differentiation.** One binary faithfully emulating
  bash/ash/ksh/zsh/csh is something upstream bash does *not* offer. That is a
  product, not just a clone.

## Cons (the ones that actually matter)

- **Bash's value is its 35-year tail of edge cases, not its code.** "74/83 parser
  agreement" and "176 identical scripts" is a strong start but a rounding error
  against real-world usage. Scripts in the wild exploit every quirk of quoting,
  signal timing, job-control races, locale/multibyte (STATUS.md flags UTF-8 as
  still TODO). The last 5% of compatibility is 95% of the work — and it never ends.
- **You are chasing a moving target forever.** bash 5.3 → 5.4 → 6.x keeps shipping.
  Parity is not a milestone; it is a permanent treadmill, and here it rests on
  essentially one maintainer.
- **The classic full-rewrite trap** (Spolsky's "never rewrite from scratch"): if the
  goal is safety, fuzzing + incremental hardening of bash likely buys more security
  per hour than reproducing every behavior anew — and new logic code means new logic
  bugs even as memory bugs vanish.
- **Adoption/trust for critical infra is brutal.** Nobody makes a young
  reimplementation their `/bin/sh` without years of hardening. The competition
  (fish, nushell, oils) mostly *avoided* competing with bash on its home turf;
  gnash meets it head-on, the hardest place to win.
- **AI-authored provenance cuts both ways.** 16 hours to a working interactive shell
  is genuinely remarkable — and a risk flag for exactly the subsystems (async
  signals, job-control races, locale) that take years, not hours, to get right.

## Score

The number swings hard on intent:

| Framed as… | Score (for) |
|---|---|
| A modular re-architecture / research artifact / a differentiated *product* (the personality shell) | **~78** |
| A serious drop-in replacement to displace or hold permanent parity with bash for general use | **~38** |
| **Blended, honest single number** | **58 / 100 — lean *for*, eyes open** |

**58.** A modest yes. The architecture, safety model, and personality feature are
real and worth continuing — this is a good *product and experiment*. What keeps it
out of the 70s is the open-ended cost of long-tail compatibility and
single-maintainer sustainability: as a strategy to *replace* bash it is a losing
bet, but as a cleaner, safer, multi-persona shell that is *its own thing*, it is
well worth the investment already made.

## Highest-leverage next move

Stop measuring against "identical to bash" on 176 curated scripts and point a
**large corpus / fuzzer** at the differential harness — bash's own test suite,
plus thousands of real-world scripts. Where gnash survives that, the 58 climbs
fast; where it does not is exactly the tail that decides whether this is a toy or
a tool.

## Corpus-scale evidence (added after the initial score)

Two corpus differentials were built and run to replace the "rounding-error"
concern with real numbers:

- **Parser differential** (`tests/harness/run_diff_corpus_parse.sh`) — `gnash-parse`
  vs `bash -n` over **429 real scripts** harvested from the system (Homebrew's
  `brew`, autoconf `configure` scripts, ffmpeg build scripts, the bash source
  tree's own scripts). Result: **429/429 = 100% accept/reject agreement** (427
  both-accept, 2 both-reject — so it is not blind leniency). Safe by construction:
  no execution.
- **Execution differential** (`tests/harness/run_diff_corpus_exec.sh` +
  `gen_corpus.sh`) — a self-authored, balanced, machine-generated corpus of **269
  snippets** run under gnash and bash in a sandbox, comparing stdout + exit status.
  Result: **264/269 = 98.1% agreement**, surfacing **4 genuine, reproducible
  divergences** (verified directly under both shells):

  | Snippet | gnash | bash |
  |---|---|---|
  | `set -- w x y z; echo ${!#}` (indirection through `$#`) | `0` | `z` |
  | `declare -A m=([a b]="c d"); echo "[${m[a b]}]"` (assoc key with space) | `[]` | `[c d]` |
  | `a=(); echo "${a[@]:-empty}"` (`:-` default on empty array) | *(empty)* | `empty` |
  | `printf "%(%Y)T\n" 0` (time-format conversion) | `%(%Y)T` | `1969` |

**What this does to the score.** The parser result directly weakens the strongest
con ("74/83 is a rounding error") — on *real* scripts the parser is at parity.
The execution result confirms the tail is real but *shallow*: ~2% divergence on
broad generated input, and the misses are named, small, and fixable rather than
architectural. Net effect: the blended score nudges from **58 → ~60**, and the
"drop-in replacement" axis rises modestly (~38 → ~44) now that parser parity on
real scripts is demonstrated rather than asserted. The four misses above are the
concrete next fixes; each closed is a measurable tick upward.

> Method note: the first execution-harness run reported spurious mismatches
> (relative binary path broken by a sandbox `cd`; a timeout watcher holding the
> capture pipe open). Both were fixed and every surviving divergence was
> re-verified directly under both shells before being reported here — a reminder
> that the harness itself must be trusted before its numbers are.

---

*Evaluation date: 2026-07-22.*
