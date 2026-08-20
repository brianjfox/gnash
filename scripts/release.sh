#!/usr/bin/env bash
# Copyright (c) 2026 Brian J. Fox
# Licensed under GPLv2 with the GPLv2-AI Exception.
#
# release.sh -- cut a gnash release and publish every artifact.
#
#   scripts/release.sh X.Y.Z [--notes FILE] [--skip-bottle] [--no-push]
#
# End to end this:
#   1. bumps the version in CMakeLists.txt and README.md,
#   2. commits "Release gnash X.Y.Z" and tags gnash-X.Y.Z, pushing both,
#   3. builds per-architecture macOS binaries and packages
#      gnash-X.Y.Z-macos-{arm64,x86_64}.tar.gz (+ .sha256),
#   4. creates the GitHub release on brianjfox/gnash with those assets,
#   5. bumps the Homebrew formula (url + source sha256) in the tap,
#   6. builds the arm64 Homebrew bottle, uploads it to the tap's release,
#   7. records the bottle sha256 in the formula and pushes the tap.
#
# Every step is idempotent: a stage whose artifact already exists is skipped,
# so a half-finished release (e.g. one that was only committed and tagged) can
# be completed by re-running with the same version.
#
# Requirements: run on the release Mac (Apple Silicon, macOS Tahoe) with git,
# gh (authenticated), brew, cmake, lipo, and shasum available, and the
# brianjfox/tools tap already tapped.

set -euo pipefail

# ---- configuration ---------------------------------------------------------
MAIN_REPO="brianjfox/gnash"                 # main source repo (releases + tags)
TAP_REPO="brianjfox/homebrew-tools"         # tap repo (formula + bottles)
TAP_SLUG="brianjfox/tools"                  # `brew tap' name of TAP_REPO
FORMULA="gnash"                             # formula / binary name
BOTTLE_TAG="${BOTTLE_TAG:-}"                # host bottle platform label (detected below)
TARBALL_EXTRA=(LICENSE.md README.md GPLv2-AI-Exception.md)  # shipped alongside the binary

# ---- argument parsing ------------------------------------------------------
VERSION=""
NOTES_FILE=""
SKIP_BOTTLE=0
DO_PUSH=1
while [ $# -gt 0 ]; do
  case "$1" in
    --notes)       NOTES_FILE=${2:?--notes needs a file}; shift 2 ;;
    --skip-bottle) SKIP_BOTTLE=1; shift ;;
    --no-push)     DO_PUSH=0; shift ;;
    -h|--help)     sed -n '5,26p' "$0"; exit 0 ;;
    -*)            echo "unknown option: $1" >&2; exit 2 ;;
    *)             if [ -z "$VERSION" ]; then VERSION=$1; shift
                   else echo "unexpected argument: $1" >&2; exit 2; fi ;;
  esac
done
[ -n "$VERSION" ] || { echo "usage: $0 X.Y.Z [--notes FILE] [--skip-bottle] [--no-push]" >&2; exit 2; }
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "version must be X.Y.Z, got: $VERSION" >&2; exit 2; }

TAG="${FORMULA}-${VERSION}"
ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
cd "$ROOT"

step()  { printf '\n\033[1;34m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
info()  { printf '    %s\n' "$*"; }
skip()  { printf '    \033[2m(skip) %s\033[0m\n' "$*"; }
die()   { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }
# Push a repo's HEAD (and optionally a tag) to origin.  A GitHub HTTPS remote
# has no non-interactive credentials, so fall back to the authenticated gh
# token; SSH remotes (git@github.com) push directly.
git_push() {  # git_push DIR [REFSPEC...]
  local dir=$1; shift
  if [ "$DO_PUSH" != 1 ]; then skip "would: git -C $dir push origin ${*:-HEAD}"; return; fi
  local url; url=$(git -C "$dir" remote get-url origin)
  case "$url" in
    https://github.com/*)
      local slug=${url#https://github.com/}; slug=${slug%.git}
      git -C "$dir" push "https://x-access-token:$(gh auth token)@github.com/$slug.git" \
        "${@:-HEAD:$(git -C "$dir" symbolic-ref --short HEAD)}"
      # A token-URL push does not move origin's remote-tracking refs, so the
      # repo then looks out of date; `brew update` later rebases the tap onto
      # a remote that already has these commits and conflicts.  Realign.
      git -C "$dir" fetch -q origin || true ;;
    *) git -C "$dir" push origin "${@:-HEAD}" ;;
  esac
}

# ---- preflight -------------------------------------------------------------
step "Preflight for $TAG"
for t in git gh brew cmake lipo shasum; do command -v "$t" >/dev/null || die "missing tool: $t"; done
gh auth status >/dev/null 2>&1 || die "gh is not authenticated (run: gh auth login)"
TAP_DIR=$(brew --repository "$TAP_SLUG" 2>/dev/null) || die "tap $TAP_SLUG not found (brew tap $TAP_SLUG)"
[ -f "$TAP_DIR/Formula/$FORMULA.rb" ] || die "formula not found: $TAP_DIR/Formula/$FORMULA.rb"
info "repo:    $ROOT"
info "tap:     $TAP_DIR"

# Resolve release notes: explicit --notes, else scripts/notes/<tag>.md, else
# a generated body (tag/commit subject bullets + the standard install footer).
if [ -z "$NOTES_FILE" ] && [ -f "scripts/notes/$TAG.md" ]; then NOTES_FILE="scripts/notes/$TAG.md"; fi
NOTES_TMP=""
if [ -n "$NOTES_FILE" ]; then
  [ -f "$NOTES_FILE" ] || die "notes file not found: $NOTES_FILE"
  info "notes:   $NOTES_FILE"
else
  NOTES_TMP=$(mktemp); NOTES_FILE="$NOTES_TMP"
  {
    echo "# $FORMULA $VERSION"; echo
    echo "## Install"; echo
    echo '```'
    echo "brew tap $TAP_SLUG && brew trust $TAP_SLUG && brew install $FORMULA"
    echo '```'; echo
    echo "Or download the macOS tarball for your architecture (arm64 for"
    echo "Apple Silicon, x86_64 for Intel) below."
  } > "$NOTES_TMP"
  info "notes:   (generated -- no scripts/notes/$TAG.md found)"
fi
trap '[ -n "$NOTES_TMP" ] && rm -f "$NOTES_TMP"' EXIT

# ---- 1-2. version bump, commit, tag, push ----------------------------------
step "Version, commit, and tag"
if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then
  skip "tag $TAG already exists"
  cur=$(sed -n 's/^  VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
  [ "$cur" = "$VERSION" ] || info "note: CMakeLists.txt says $cur, not $VERSION (tag already cut)"
else
  [ -z "$(git status --porcelain)" ] || die "working tree not clean; commit or stash first"
  sed -i '' -E "s/^(  VERSION )[0-9]+\.[0-9]+\.[0-9]+/\1$VERSION/" CMakeLists.txt
  sed -i '' -E "s/(GNASH_REF=${FORMULA}-)[0-9]+\.[0-9]+\.[0-9]+/\1$VERSION/" README.md
  git grep -q "VERSION $VERSION" -- CMakeLists.txt || die "CMakeLists.txt bump failed"
  git add CMakeLists.txt README.md
  git commit -q -m "chore(release): $FORMULA $VERSION" \
    -m "See scripts/notes/$TAG.md for release notes."
  git tag -a "$TAG" -m "$FORMULA $VERSION"
  info "committed and tagged $TAG"
  git_push "$ROOT"
  git_push "$ROOT" "$TAG"
fi

# ---- 3. per-architecture macOS tarballs -------------------------------------
step "Build per-architecture macOS binaries"
STAGE="$ROOT/dist"
mkdir -p "$STAGE"
ASSETS=()
for ARCH in arm64 x86_64; do
  BUILD="$ROOT/build-release-$ARCH"
  PKG="$FORMULA-$VERSION-macos-$ARCH"
  TARBALL="$STAGE/$PKG.tar.gz"
  ASSETS+=("$TARBALL" "$TARBALL.sha256")
  if [ -f "$TARBALL" ] && [ -f "$TARBALL.sha256" ]; then
    skip "$PKG.tar.gz already built"
    continue
  fi
  rm -rf "$BUILD"
  cmake -S "$ROOT" -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
    -DGNASH_BUILD_TESTS=OFF -DGNASH_WERROR=OFF >/dev/null
  cmake --build "$BUILD" --target "$FORMULA" -j >/dev/null
  BIN="$BUILD/core/$FORMULA"
  archs=$(lipo -archs "$BIN")
  [ "$archs" = "$ARCH" ] || die "binary is not $ARCH-only (got: $archs)"
  strip -x "$BIN" 2>/dev/null || true
  rm -rf "$STAGE/$PKG"; mkdir -p "$STAGE/$PKG"
  cp "$BIN" "$STAGE/$PKG/"
  for f in "${TARBALL_EXTRA[@]}"; do cp "$ROOT/$f" "$STAGE/$PKG/"; done
  tar -C "$STAGE" -czf "$TARBALL" "$PKG"
  ( cd "$STAGE" && shasum -a 256 "$PKG.tar.gz" > "$PKG.tar.gz.sha256" )
  rm -rf "$STAGE/$PKG"
  info "packaged $PKG.tar.gz ($(cd "$STAGE" && du -h "$PKG.tar.gz" | cut -f1)), $archs"
done

# ---- 4. GitHub release on the main repo ------------------------------------
step "GitHub release on $MAIN_REPO"
if gh release view "$TAG" --repo "$MAIN_REPO" >/dev/null 2>&1; then
  skip "release $TAG exists -- refreshing assets"
  # A release published without assets may be IMMUTABLE (GitHub's immutable
  # releases): uploads then fail with HTTP 422.  That is not fatal to the
  # release -- the tap formula builds from the source tarball -- so warn and
  # carry on to the Homebrew steps rather than aborting the whole run.
  if ! gh release upload "$TAG" "${ASSETS[@]}" \
         --repo "$MAIN_REPO" --clobber 2>/tmp/gnash-relup.$$; then
    info "warning: could not attach assets to $TAG ($(tr -d '\n' </tmp/gnash-relup.$$ | tail -c 120))"
    info "         the release stands; Homebrew installs from the source tarball"
  fi
  rm -f /tmp/gnash-relup.$$
else
  gh release create "$TAG" "${ASSETS[@]}" \
    --repo "$MAIN_REPO" --title "$FORMULA $VERSION" --notes-file "$NOTES_FILE"
  info "created release $TAG"
fi

# ---- 5. bump formula url + source sha256 -----------------------------------
step "Update tap formula (url + source sha256)"
SRC_URL="https://github.com/$MAIN_REPO/archive/refs/tags/$TAG.tar.gz"
SRC_SHA=$(curl -fsSL "$SRC_URL" | shasum -a 256 | cut -d' ' -f1)
[ ${#SRC_SHA} -eq 64 ] || die "could not compute source sha256 for $SRC_URL"
info "source sha256: $SRC_SHA"
python3 "$ROOT/scripts/formula_edit.py" "$TAP_DIR/Formula/$FORMULA.rb" \
  set-source --url "$SRC_URL" --sha "$SRC_SHA"
if git -C "$TAP_DIR" diff --quiet -- "Formula/$FORMULA.rb"; then
  skip "formula already at $TAG source"
else
  git -C "$TAP_DIR" add "Formula/$FORMULA.rb"
  git -C "$TAP_DIR" commit -q -m "$FORMULA $VERSION"
  info "committed formula url/sha bump"
fi

# ---- 6. tap release + Homebrew bottle --------------------------------------
# The bottle platform label (arm64_tahoe, arm64_golden_gate, ...) changes with
# every macOS release, so ask Homebrew rather than hard-coding it: a stale value
# only surfaces after the build, as "brew bottle produced no tarball".  $BOTTLE_TAG
# from the environment still wins, for cross-checking or a fixed CI label.
if [ -z "$BOTTLE_TAG" ] && [ "$SKIP_BOTTLE" != 1 ]; then
  BOTTLE_TAG=$(brew ruby -e 'puts "#{Hardware::CPU.arch}_#{MacOS.version.to_sym}"' \
                 2>/dev/null | tail -1)
  [ -n "$BOTTLE_TAG" ] || die "cannot determine the bottle platform; set BOTTLE_TAG"
fi
BOTTLE_ASSET="$FORMULA-$VERSION.$BOTTLE_TAG.bottle.tar.gz"
if [ "$SKIP_BOTTLE" = 1 ]; then
  step "Bottle (skipped: --skip-bottle)"
else
  step "Build and upload $BOTTLE_TAG bottle"
  if ! gh release view "$TAG" --repo "$TAP_REPO" >/dev/null 2>&1; then
    gh release create "$TAG" --repo "$TAP_REPO" \
      --title "$FORMULA $VERSION bottles" \
      --notes "Homebrew bottles for \`$FORMULA $VERSION\`."
    info "created tap release $TAG"
  fi
  if gh release view "$TAG" --repo "$TAP_REPO" --json assets \
       --jq '.assets[].name' 2>/dev/null | grep -qx "$BOTTLE_ASSET"; then
    skip "bottle $BOTTLE_ASSET already uploaded"
    BOTTLE_SHA=$(gh release view "$TAG" --repo "$TAP_REPO" \
      --json assets --jq ".assets[] | select(.name==\"$BOTTLE_ASSET\") | .digest" \
      2>/dev/null | sed 's/^sha256://')
  fi
  if [ -z "${BOTTLE_SHA:-}" ]; then
    ROOT_URL="https://github.com/$TAP_REPO/releases/download/$TAG"
    WORK=$(mktemp -d)
    info "building bottle in $WORK (brew install --build-bottle)"
    brew uninstall --force "$FORMULA" >/dev/null 2>&1 || true
    brew install --build-bottle "$TAP_SLUG/$FORMULA"
    ( cd "$WORK" && brew bottle --json --no-rebuild \
        --root-url="$ROOT_URL" "$TAP_SLUG/$FORMULA" )
    built=$(ls "$WORK"/$FORMULA--$VERSION.$BOTTLE_TAG.bottle.tar.gz 2>/dev/null | head -1)
    if [ -z "$built" ]; then
      # The file Homebrew actually wrote is authoritative: adopt its platform
      # label rather than failing, so a detection that disagrees still works.
      built=$(ls "$WORK"/$FORMULA--$VERSION.*.bottle.tar.gz 2>/dev/null | head -1)
      [ -n "$built" ] || die "brew bottle produced no tarball in $WORK"
      BOTTLE_TAG=$(basename "$built" | sed "s/^$FORMULA--$VERSION\\.//;s/\\.bottle\\.tar\\.gz$//")
      BOTTLE_ASSET="$FORMULA-$VERSION.$BOTTLE_TAG.bottle.tar.gz"
      info "bottle platform is $BOTTLE_TAG"
    fi
    # Homebrew names the local file with `--'; the hosted asset uses a single `-'.
    cp "$built" "$WORK/$BOTTLE_ASSET"
    BOTTLE_SHA=$(shasum -a 256 "$WORK/$BOTTLE_ASSET" | cut -d' ' -f1)
    gh release upload "$TAG" "$WORK/$BOTTLE_ASSET" --repo "$TAP_REPO" --clobber
    info "uploaded $BOTTLE_ASSET"
    rm -rf "$WORK"
  fi
  [ ${#BOTTLE_SHA} -eq 64 ] || die "bad bottle sha256: ${BOTTLE_SHA:-<empty>}"
  info "bottle sha256: $BOTTLE_SHA"

  # ---- 7. record bottle in the formula -------------------------------------
  step "Record bottle in formula and push tap"
  python3 "$ROOT/scripts/formula_edit.py" "$TAP_DIR/Formula/$FORMULA.rb" \
    set-bottle --root-url "https://github.com/$TAP_REPO/releases/download/$TAG" \
    --tag "$BOTTLE_TAG" --sha "$BOTTLE_SHA" --formula-name "$FORMULA"
  if git -C "$TAP_DIR" diff --quiet -- "Formula/$FORMULA.rb"; then
    skip "formula already records this bottle"
  else
    git -C "$TAP_DIR" add "Formula/$FORMULA.rb"
    git -C "$TAP_DIR" commit -q -m "$FORMULA: add $BOTTLE_TAG bottle ($VERSION)"
    info "committed bottle sha"
  fi
fi

# ---- push the tap ----------------------------------------------------------
step "Push tap $TAP_REPO"
git_push "$TAP_DIR"

# ---- 8. repour: leave this machine on the poured bottle --------------------
# The `brew install --build-bottle' above leaves a from-source install whose
# receipt (built_as_bottle, poured_from_bottle: false) makes every later
# install/reinstall/upgrade on this machine compile from source again and
# skip post_install (#671).  Swap it for the bottle that was just uploaded
# and recorded in the formula.
if [ "$SKIP_BOTTLE" != 1 ]; then
  step "Reinstall $FORMULA from the new bottle"
  brew uninstall --force "$FORMULA" >/dev/null 2>&1 || true
  brew install "$TAP_SLUG/$FORMULA"
fi

step "Done: $TAG released"
info "main release: https://github.com/$MAIN_REPO/releases/tag/$TAG"
[ "$SKIP_BOTTLE" = 1 ] || info "bottle:       https://github.com/$TAP_REPO/releases/tag/$TAG"
info "verify with:  brew update && brew upgrade $FORMULA"
