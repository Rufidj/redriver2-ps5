#!/bin/bash
# Builds the Vulkan title: the game library, a title made from mihawk-99's PS5 Vulkan Template, this repository's
# overlay on top of it, and the link with RADV.
# Result: $VK_TITLE_DIR/dist/$VK_TITLE_ID/  (eboot.bin, sce_sys, shaders ...); the game's data goes in its assets/.
set -e
cd "$(dirname "$0")/.."
REPO=$(pwd)
. scripts/env.sh
: "${NAB:?}" "${PS5_VULKAN_DIR:?}" "${VK_TEMPLATE_DIR:?}" "${VK_TITLE_DIR:?}"
VK_TITLE_ID=${VK_TITLE_ID:-PPSA00058}

# 1. the game library
[ -f ps5/build/libred2.a ] || ./scripts/build-game-lib.sh

# 2. the title: the template's foundation renamed, with git identity for its first commit
if [ ! -d "$VK_TITLE_DIR/.git" ]; then
	GIT_AUTHOR_NAME=${GIT_AUTHOR_NAME:-redriver2-ps5} GIT_AUTHOR_EMAIL=${GIT_AUTHOR_EMAIL:-none@example.com} \
	GIT_COMMITTER_NAME=${GIT_AUTHOR_NAME:-redriver2-ps5} GIT_COMMITTER_EMAIL=${GIT_AUTHOR_EMAIL:-none@example.com} \
	python3 "$VK_TEMPLATE_DIR/ps5/tools/new-title.py" "$VK_TITLE_DIR" --title-id "$VK_TITLE_ID" --name "REDRIVER 2" --refresh 60
	# the overlay: this port's files, and the few template files it changes
	git -C "$VK_TITLE_DIR" apply "$REPO/vulkan/template-changes.patch"
fi
cp -a vulkan/overlay/. "$VK_TITLE_DIR/"

# the title's artwork, if you made it (icon0.png, pic0.dds, pic1.dds, snd0.at9: see the README)
[ -d vulkan/sce_sys ] && cp -a vulkan/sce_sys/. "$VK_TITLE_DIR/ps5/sce_sys/"

# 3. shaders, the game library for this title, and the link
cd "$VK_TITLE_DIR"
export PS5_PAYLOAD_SDK=${PS5_PAYLOAD_SDK:-$PS5_VULKAN_DIR/.deps/native/ps5-payload-sdk}
export RED2_REPO="$REPO"
ps5/tools/compile-shaders.sh driver2vulkan
game/mklib.sh
export RED2_EXTRA_ARCHIVES="$SDL2_SDK/lib/libSDL2.a"
touch ps5/src/main.cpp
ps5/tools/build.sh
echo "built: $VK_TITLE_DIR/dist/$VK_TITLE_ID"
