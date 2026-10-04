#!/bin/bash
# Regenerates the two patches from the working trees in REDRIVER2/ (for whoever keeps developing the port).
set -e
cd "$(dirname "$0")/.."
P=$(pwd)/patches
( cd REDRIVER2
  git diff -- . ':!src_rebuild/PsyCross' > "$P/0001-redriver2-ps5-game.patch"
  for f in $(git ls-files --others --exclude-standard); do git diff --no-index /dev/null "$f" >> "$P/0001-redriver2-ps5-game.patch" || true; done )
( cd REDRIVER2/src_rebuild/PsyCross
  git diff > "$P/0002-psycross-ps5.patch"
  for f in $(git ls-files --others --exclude-standard); do git diff --no-index /dev/null "$f" >> "$P/0002-psycross-ps5.patch" || true; done )
echo "patches written to $P"
