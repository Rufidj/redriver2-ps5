#!/bin/bash
# Fetches the upstream sources at the exact commits the patches were made against and applies the PS5 patches.
set -e
cd "$(dirname "$0")/.."
REDRIVER2_COMMIT=b2d8857426f815007486def9bd23f9a6007a2c64
PSYCROSS_COMMIT=e56e4cde1c2b8a15e0d4e38b26cdd9202e0d17e6

if [ ! -d REDRIVER2/.git ]; then
	git clone https://github.com/OpenDriver2/REDRIVER2 REDRIVER2
fi
git -C REDRIVER2 checkout "$REDRIVER2_COMMIT"
git -C REDRIVER2 submodule update --init src_rebuild/PsyCross
git -C REDRIVER2/src_rebuild/PsyCross checkout "$PSYCROSS_COMMIT"

# already applied?
if git -C REDRIVER2 apply --check -R ../patches/0001-redriver2-ps5-game.patch 2>/dev/null; then
	echo "patches already applied"
else
	git -C REDRIVER2 apply ../patches/0001-redriver2-ps5-game.patch
	git -C REDRIVER2/src_rebuild/PsyCross apply ../../../patches/0002-psycross-ps5.patch
	echo "patches applied"
fi
