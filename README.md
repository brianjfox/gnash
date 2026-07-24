# gnash

A modular C++ reimplementation of the **GNU Bash** shell, with multiple personalities.

## Rationale
When humans used to write software, the time and effort to create something was much larger than today.  This often created competition between different versions of the same functional software, where people would declare that they liked feature X over feature Y, or that this peice of software was better at doing X.  This kind of religious fervor over which software was *better*, led to operating system distributors having to choose which software would be the default on their systems.

Sometimes, people were confused by licensing, and chose software based on how lenient the licensing was.  **gnash** is an attempt to make all of this nonsense go away, and simply deliver the functionality and features of multiple different shells in the same software package.

I had multiple goals in mind when creating gnash:

1. Create a replacement shell that makes the reasons for having different shells go away. When running scripts, `gnash` should behave *identically* to the personality it is invoked as: **bash**, **ash**, **ksh**, **zsh**, or even **csh**/**tcsh** -- the same stdout/stderr, exit status, side effects, and error semantics.
2. I wanted to connect people to the fact that the relationship of humans to software is changing drastically.  Humans still need to be the motivating factor behind the existance of the software, and often, we have architectural goals that are larger than a single, or even a suite, of software.  But writing clean and efficient code is no longer the purview of meat-people.  gnash was conceived of, designed, and written in approximately 6 hours of human attention coupled with 10 hours of computational coding.

**zsh** has an *emulate* builtin, but that builtin specifically does not emulate bash, nor are the emulations feature-complete.  Because of the modular nature of the gnash shell, we are able to attempt to faithfully emulate other shells, including tcsh and zsh.

## How to Install It

If you don't want to build from source yourself, install a released version.

### macOS or Linux — Homebrew (recommended)

Homebrew runs on both macOS and Linux, and the same tap works on either:

```sh
brew tap brianjfox/tools
brew trust brianjfox/tools
brew install gnash
```

`brew trust` is needed because gnash comes from a third-party tap: recent
Homebrew won't load formulae from an untrusted tap. Later, `brew upgrade gnash`
picks up new releases.

On macOS Apple Silicon a prebuilt **bottle** is poured (no compiler needed). On
other platforms — or if you'd rather compile it yourself — build from source:

```sh
brew install --build-from-source gnash
```

That needs a C++20 compiler and CMake (`brew install cmake`); Homebrew fetches
the release source tarball and runs the same CMake build.

### Linux or macOS — install script

No Homebrew? This one-liner downloads the latest release, builds it from source,
and installs the `gnash` binary:

```sh
curl -fsSL https://raw.githubusercontent.com/brianjfox/gnash/main/install.sh | bash
```

It needs a C++20 compiler and CMake ≥ 3.16 — install them first if needed
(`sudo apt install build-essential cmake`, `sudo dnf install gcc-c++ cmake`,
`sudo pacman -S base-devel cmake`, or `brew install cmake`). By default it
installs to `/usr/local/bin`, falling back to `~/.local/bin`; override with
`PREFIX=~/somewhere`, or pin a version with `GNASH_REF=gnash-1.5.0`.

Prefer to look before you leap? The script is [`install.sh`](install.sh) — read
it, then run `bash install.sh`.

### Prebuilt macOS binary

Every [release](https://github.com/brianjfox/gnash/releases) also ships a
universal (Apple Silicon + Intel) macOS binary tarball, if you'd rather not use
Homebrew or build anything.

## How to Build It
**gnash** needs a C++20 compiler and CMake ≥ 3.16.

- **macOS** — install the Xcode command-line tools and CMake:
  `xcode-select --install`, then `brew install cmake`.
- **Linux** — install a C++20 compiler, CMake, and Make:
  `sudo apt install build-essential cmake` (Debian/Ubuntu) or
  `sudo dnf install gcc-c++ cmake make` (Fedora/RHEL).
- **Windows** — build under **WSL** (Windows Subsystem for Linux) and follow the
  Linux steps; **MSYS2** or **Cygwin** also work. A native MSVC build is not
  supported: gnash relies on POSIX process/terminal APIs (`fork`, `termios`,
  `tcsetpgrp`, `setpgid`, …) that Windows lacks outside those environments.

Then, on any platform:

```bash
cmake -S . -B build -DGNASH_WERROR=ON
cmake --build build -j
```

## How to Run It
**gnash** can be run under many different personalities.  The name of the binary (or the value of the `--personality=XXX` option) controls which startup files are read, whether syntax highlighting on the command line exists, and other behaviors.  Currently, **gnash** supports running as:

* **bash** -- reads `~/.bash_profile`, `~/.bashrc`, behaves like **bash-5.3**
* **gnash** -- reads `~/.gnash_profile`, `~/.gnashrc` (falling back to `~/.bash_profile`, `~/.bashrc` when the gnash-named files are absent), behaves like **bash-5.3**
* **zsh** -- reads `~/.zshenv`, `~/.zprofile`, `~/.zshrc`, `~/.zlogin`, uses `%`-style prompts and highlights the command line as you type, zsh-style tab completion, behaves like **zsh-5.1**
* **ash** (also **dash**, **sh**) -- reads `/etc/profile`, `~/.profile`, and `$ENV`, uses a plain POSIX `$ ` prompt, behaves like **bash-5.3**
* **ksh** (also **ksh93**, **mksh**, **pdksh**) -- reads `/etc/profile`, `~/.profile`, and `$ENV`, uses a `$ ` prompt in which `!` expands to the history number, behaves like **bash-5.3**
* **csh** (also **tcsh**) -- reads `/etc/csh.cshrc`, `~/.cshrc` (or `~/.tcshrc`), and `~/.login`, and runs the **C shell language** itself: `set`/`@`/`setenv`, `$var[n]` (1-indexed), `$#var`, `$?var`, `` `cmd` ``, `:h`/`:t`/`:r`/`:e` modifiers, `if/then/else/endif`, `foreach`, `while`, and `switch/case/endsw`. Unlike the other personalities (which are surface behavior over the shared Bourne engine), csh is a genuinely different language, so it has its own lexer, parser, and evaluator. Verified against **tcsh 6.21**.

Choose a personality by the binary's name -- e.g. a `zsh` symlink to `gnash`, or a copy named `ksh` -- or with the `--personality=<name>` option, which takes precedence over the name. The active personality is exposed in `$GNASH_PERSONALITY`.

Run it as `build/core/gnash` (interactive), `gnash -c 'CMD'`, or `gnash SCRIPT`.


## Design

gnash is built as a stack of independent libraries, lowest-dependency-first, each a
separate CMake target. Modularity is enforced through target link visibility, so the
dependency graph can't silently erode.

```
libsh        low-level utilities (alloc, quoting, shell-env seam)
  ├─ libhistory     GNU History reimplementation
  ├─ libtilde       tilde expansion
  ├─ libtermcap     terminal capabilities
  ├─ libreadline    line editing + completion hooks
  └─ libglob        pattern matching + filename globbing
core   shell: parse → expand → execute + jobs + REPL    
```

Each library exposes a **modern C++ API** under `namespace gnash::*` (headers in
`include/gnash/`) plus a **thin C shim** with the classic names and ABI (headers in
`compat/`), so existing programs can link a drop-in replacement.

## Status

Per-module status — what each library and the core shell implement — lives in
[STATUS.md](STATUS.md), updated as features and modules land.

## Build & test

`cmake` is not always on `PATH`; any CMake ≥ 3.16 works.

```sh
cmake -S . -B build -G "Unix Makefiles" -DGNASH_WERROR=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Options: `-DGNASH_SANITIZE=ON` (ASan/UBSan), `-DGNASH_BUILD_TESTS=OFF`.

## Conformance

Six harnesses:

- **Differential** (`tests/harness/run_diff.sh`, gated in ctest) — runs a growing corpus
  of scripts under both gnash and bash 5.3 and requires identical stdout + exit status.
  Currently **221 scripts** covering expansion, arithmetic, control flow, arrays, functions,
  redirection, process substitution, the special variables, the full builtin set, and job
  control — all matching.
- **csh differential** (`tests/harness/run_diff_csh.sh`, gated in ctest when `tcsh` is
  installed) — runs a corpus of C-shell scripts under gnash's csh personality and under a
  reference **tcsh 6.21**, requiring identical stdout + exit status. Currently **36 scripts**
  covering `set`/`@` arithmetic, list variables and indexing, `:` modifiers, backquotes,
  `if`/`foreach`/`while`/`switch`, pipelines and redirection — all matching.
- **zsh differential** (`tests/harness/run_diff_zsh.sh`, gated in ctest when `zsh` is
  installed) — runs a corpus under gnash's zsh personality and under a reference **zsh**,
  requiring identical stdout + exit status. Currently **43 scripts** exercising where zsh
  diverges from bash: word splitting, bare-array expansion, 1-based array indexing and
  ranges, the `${(flags)…}` expansion flags, `${=name}` splitting, scalar indexing, and
  associative arrays — all matching.
- **ksh differential** (`tests/harness/run_diff_ksh.sh`, gated in ctest when `ksh` is
  installed) — runs a corpus under gnash's ksh personality and under a reference **ksh93**,
  requiring identical stdout + exit status. Currently **37 scripts** covering arithmetic,
  parameter expansion, arrays, `typeset` attributes, `((…))`/`let`, control flow, `[[ ]]`,
  and the common ksh/POSIX builtins — all matching.
- **Error format** (`tests/harness/errfmt.sh`, gated in ctest) — diffs stderr for a set of
  error cases against bash, normalizing the leading program name. gnash now emits bash's
  exact `name: line N: context: message` format; the only difference is the program name
  (`gnash` vs `bash`), which is inherent.
- **bash test suite** (`tests/harness/conformance.sh`) — runs bash's own `tests/*.tests`
  under gnash (with `$THIS_SH`/env set up like bash's `run-all`, and the `recho`/`zecho`
  helpers built) and diffs against the checked-in `.right` files. This is a *progress
  metric*, not a hard gate: many `.right` files capture the exact program name in error text
  and self-test summaries, so a behaviourally-correct gnash still diverges on those lines.
  The differential and error-format harnesses (individual constructs cross-checked against
  real bash) are the more representative signal. The tests gnash reproduces exactly are
  pinned as a ctest regression gate (`conformance_gate.sh`). The readline-domain test
  files — `read.tests`, `histexp.tests`, and `history.tests` — produce output
  byte-identical to bash 5.3's when both shells run them in the same environment
  (modulo the program name and a run-varying pid in one job-control warning).
