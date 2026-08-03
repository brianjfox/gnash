#!/usr/bin/env bash
# Copyright (c) 2026 Brian J. Fox
# Licensed under GPLv2 with the GPLv2-AI Exception.
#
# corpus-fetch.sh -- assemble a large real-world shell-script corpus by shallow-
# cloning a curated set of shell-heavy projects, for use with corpus-diff.sh:
#
#   scripts/corpus-fetch.sh [DEST]        # default DEST: ./corpus (gitignored)
#   scripts/corpus-diff.sh --parse DEST
#
# Each repo is cloned --depth 1 (fast, no history) and skipped if already
# present.  These are chosen for a high density of portable /bin/sh and
# /bin/bash scripts -- build glue, completions, test harnesses, version
# managers -- i.e. exactly the code a bash replacement must parse and run.
set -uo pipefail

DEST=${1:-"$(cd "$(dirname "$0")/.." && pwd)/corpus"}
mkdir -p "$DEST"

# name<TAB>url  (shallow-cloned into DEST/name)
REPOS='
git	https://github.com/git/git
bats-core	https://github.com/bats-core/bats-core
bash-completion	https://github.com/scop/bash-completion
shellcheck	https://github.com/koalaman/shellcheck
nvm	https://github.com/nvm-sh/nvm
rbenv	https://github.com/rbenv/rbenv
pyenv	https://github.com/pyenv/pyenv
autoconf	https://github.com/autotools-mirror/autoconf
gnulib	https://github.com/coreutils/gnulib
dotbot	https://github.com/anishathalye/dotbot
tldr	https://github.com/tldr-pages/tldr
docker-install	https://github.com/docker/docker-install
'

echo "corpus destination: $DEST" >&2
printf '%s\n' "$REPOS" | while IFS=$'\t' read -r name url; do
  [ -n "${name:-}" ] || continue
  if [ -d "$DEST/$name/.git" ]; then
    echo "  have  $name" >&2
    continue
  fi
  echo "  clone $name ($url)" >&2
  git clone --depth 1 --quiet "$url" "$DEST/$name" 2>/dev/null \
    && echo "        ok" >&2 \
    || echo "        FAILED (skipped)" >&2
done

# Count what the corpus scanner will actually pick up.
n=$(find "$DEST" -type f \( -name '*.sh' -o -name '*.bash' \) 2>/dev/null | wc -l | tr -d ' ')
echo "corpus ready: $DEST  (~$n *.sh/*.bash files, plus shebang scripts)" >&2
