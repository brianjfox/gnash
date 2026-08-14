# gnash 2.1.3

_(draft — summarize the release here before cutting it)_

## Build

- macOS releases now ship per-architecture binary tarballs
  (`gnash-X.Y.Z-macos-arm64.tar.gz` for Apple Silicon,
  `gnash-X.Y.Z-macos-x86_64.tar.gz` for Intel) instead of a single
  universal tarball, roughly halving the download (#667). Homebrew
  installs are unaffected.

## Install

```
brew tap brianjfox/tools && brew trust brianjfox/tools && brew install gnash
```

Or download the macOS tarball for your architecture (arm64 for
Apple Silicon, x86_64 for Intel) below.
