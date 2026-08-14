# gnash 2.1.3

A patch release: the `personality` builtin's advertised options now all
work as documented, and macOS release downloads are half the size.

## Fixes

- The `personality` builtin implements the options its help always
  advertised (#660): `-l` lists the available personalities without
  switching (it was silently ignored, so `personality -l sh` switched);
  `-R` resets to the personality the shell was invoked with; `-c` no
  longer requires a personality name (`personality -c echo $SHELL` runs
  under the current one) and takes the rest of the words as the command
  line instead of quietly ignoring them — a bare `-c` and stray extra
  arguments are now errors. `-L` keeps its zsh-compatible
  local-to-function switch, and the help text now describes the real
  behavior.

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
