#!/usr/bin/env bash
# Copyright (c) 2026 Brian J. Fox
# Licensed under GPLv2 with the GPLv2-AI Exception.

# gnash installer -- downloads the latest release, builds it from source, and
# installs the `gnash' binary.  Works on Linux and macOS with a C++20 compiler
# and CMake.
#
#   curl -fsSL https://raw.githubusercontent.com/brianjfox/gnash/main/install.sh | bash
#
# Environment:
#   PREFIX=<dir>   install to <dir>/bin (default: /usr/local, or ~/.local if
#                  /usr/local is not writable)
#   GNASH_REF=<tag|branch>  build a specific tag or branch (default: latest release)

set -euo pipefail

REPO="brianjfox/gnash"

say()  { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# ---- prerequisites --------------------------------------------------------
command -v curl  >/dev/null 2>&1 || die "curl is required"
command -v tar   >/dev/null 2>&1 || die "tar is required"
command -v cmake >/dev/null 2>&1 || die "cmake (>= 3.16) is required -- e.g. \
'sudo apt install cmake', 'sudo dnf install cmake', 'sudo pacman -S cmake', or 'brew install cmake'"
if ! command -v c++ >/dev/null 2>&1 && ! command -v g++ >/dev/null 2>&1 && \
   ! command -v clang++ >/dev/null 2>&1; then
  die "a C++20 compiler is required -- e.g. 'sudo apt install build-essential', \
'sudo dnf install gcc-c++', or Xcode command-line tools on macOS"
fi

# ---- which version --------------------------------------------------------
REF="${GNASH_REF:-}"
if [ -z "$REF" ]; then
  say "Finding the latest gnash release..."
  REF="$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" \
        | sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' | head -n1)"
  [ -n "$REF" ] || die "could not determine the latest release; set GNASH_REF=<tag>"
fi
say "Installing gnash ($REF)"

# ---- download + build -----------------------------------------------------
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
say "Downloading source..."
curl -fsSL "https://github.com/$REPO/archive/$REF.tar.gz" | tar -xz -C "$TMP"
SRC="$(find "$TMP" -maxdepth 1 -type d -name 'gnash-*' | head -n1)"
[ -n "$SRC" ] || die "unexpected source archive layout"

say "Building (this takes ~10-60s)..."
cmake -S "$SRC" -B "$TMP/build" \
  -DCMAKE_BUILD_TYPE=Release -DGNASH_WERROR=OFF -DGNASH_BUILD_TESTS=OFF >/dev/null
cmake --build "$TMP/build" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" >/dev/null
BIN="$TMP/build/core/gnash"
[ -x "$BIN" ] || die "build did not produce a gnash binary"

# ---- install --------------------------------------------------------------
if [ -n "${PREFIX:-}" ]; then
  DEST="$PREFIX/bin"
elif [ "$(id -u)" = 0 ]; then
  DEST="/usr/local/bin"
elif [ -w /usr/local/bin ]; then
  DEST="/usr/local/bin"
else
  DEST="$HOME/.local/bin"
fi
mkdir -p "$DEST" 2>/dev/null || true

SUDO=""
if [ ! -w "$DEST" ]; then
  if command -v sudo >/dev/null 2>&1; then SUDO="sudo"; else
    die "cannot write to $DEST -- rerun with PREFIX=\$HOME/.local or as root"
  fi
fi

say "Installing to $DEST/gnash"
$SUDO install -m 0755 "$BIN" "$DEST/gnash"

say "Installed gnash ($REF) -> $DEST/gnash"
"$DEST/gnash" -c 'printf "gnash is ready: %s\n" "$BASH_VERSION"' || true
case ":$PATH:" in
  *":$DEST:"*) ;;
  *) warn "$DEST is not on your PATH; add it with:  export PATH=\"$DEST:\$PATH\"" ;;
esac
