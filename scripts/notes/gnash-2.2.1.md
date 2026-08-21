# gnash 2.2.1

A patch release: Homebrew bottles and macOS tarballs now run on macOS 13
and later, and bottles pour on every supported Apple Silicon macOS —
`brew upgrade` no longer falls back to compiling from source on machines
older than the release host (#690).

## Build

- All published macOS binaries are built with a macOS 13.0 deployment
  target. Previous releases carried the release host's version as their
  minimum (macOS 27), so they could not run — or pour — anywhere else.
- The (self-contained, relocation-free) bottle is published under the
  `arm64_sonoma`, `arm64_sequoia`, `arm64_tahoe` and `arm64_golden_gate`
  labels, so Apple Silicon machines on macOS 14–27 all pour the same
  binary instead of building from source.
- `scripts/release.sh` verifies every published binary's minimum-macOS
  version before uploading.

No shell behavior changes since 2.2.0.

## Install

```
brew tap brianjfox/tools && brew trust brianjfox/tools && brew install gnash
```

Or download the macOS tarball for your architecture (arm64 for
Apple Silicon, x86_64 for Intel) below.
