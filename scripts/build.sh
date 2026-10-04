#!/bin/bash
# Builds the game library, the support library and links the PS5 title.
# Result: $NAB/dist/$TITLE_ID/ (eboot.bin, sce_sys/, assets/ ...)
set -e
cd "$(dirname "$0")/.."
REPO=$(pwd)
. scripts/env.sh
: "${NAB:?}" "${PS5_OPENGL_SDK:?}" "${SDL2_SDK:?}" "${PAYLOAD_LIB:?}" "${TITLE_ID:=PPSA00056}"

# 1. the boilerplate expects everything it links under its own tree, relative to its root
mkdir -p "$NAB/apps" "$NAB/vendor/extras" "$NAB/vendor/payload-sdk-cxx"
ln -sfn "$REPO/ps5/app" "$NAB/apps/redriver2"
ln -sfn "$PS5_OPENGL_SDK" "$NAB/vendor/ps5-opengl-sdk"
ln -sfn "$SDL2_SDK" "$NAB/vendor/sdl2-real"
for l in libc++.a:libcxx.a libc++abi.a:libcxxabi.a libunwind.a:libunwind.a; do
	ln -sfn "$PAYLOAD_LIB/${l%%:*}" "$NAB/vendor/payload-sdk-cxx/${l##*:}"
done
mkdir -p ps5/app/libs ps5/app/assets
[ -f ps5/app/assets/config.ini ] || cp ps5/assets/config.ini ps5/app/assets/config.ini

# 2. libextras.a = LLVM emutls (the OpenGL SDK uses emulated thread-locals)
export PS5_PAYLOAD_SDK="$NAB/.deps/native/ps5-payload-sdk"
CC="sh $NAB/tooling/prospero-clang18"
$CC -O2 -c ps5/build/third_party/emutls.c -o "$NAB/vendor/extras/emutls.c.o"
rm -f "$NAB/vendor/extras/libextras.a" && ar rcs "$NAB/vendor/extras/libextras.a" "$NAB/vendor/extras/emutls.c.o"

# 3. the game (REDRIVER2 + PsyCross) as one static library
make -C ps5/build -j"$(nproc)" NAB="$NAB"
cp ps5/build/libred2.a ps5/app/libs/libred2.a

# 4. link the title
cd "$NAB"
make app USE_CCACHE=0 \
	APP_SOURCE_DIR=apps/redriver2/src \
	APP_PARAM=apps/redriver2/sce_sys/param.json \
	APP_SCE_SYS=apps/redriver2/sce_sys \
	APP_ASSETS=apps/redriver2/assets \
	TITLE_ID="$TITLE_ID" \
	APP_STATIC_ARCHIVES="apps/redriver2/libs/libred2.a vendor/sdl2-real/lib/libSDL2.a vendor/ps5-opengl-sdk/lib/libPS5OpenGL.a vendor/payload-sdk-cxx/libcxx.a vendor/payload-sdk-cxx/libcxxabi.a vendor/payload-sdk-cxx/libunwind.a vendor/extras/libextras.a" \
	APP_INCLUDE_PATHS=vendor/ps5-opengl-sdk/include \
	APP_DEFINITIONS=GL_GLEXT_PROTOTYPES=1 \
	APP_WRAP_SYMBOLS="malloc calloc realloc free posix_memalign malloc_usable_size fopen mkdir"
echo "built: $NAB/dist/$TITLE_ID"
