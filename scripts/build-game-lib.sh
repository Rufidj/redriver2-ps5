#!/bin/bash
# Builds the game (REDRIVER2 + PsyCross) as one static library: ps5/build/libred2.a.
# Both targets use it: the OpenGL title (scripts/build.sh) and the Vulkan title (scripts/build-vulkan.sh).
set -e
cd "$(dirname "$0")/.."
REPO=$(pwd)
. scripts/env.sh
: "${NAB:?}" "${PS5_OPENGL_SDK:?}" "${SDL2_SDK:?}" "${PAYLOAD_LIB:?}"

# the boilerplate's toolchain expects everything it links under its own tree
mkdir -p "$NAB/vendor/extras" "$NAB/vendor/payload-sdk-cxx"
ln -sfn "$PS5_OPENGL_SDK" "$NAB/vendor/ps5-opengl-sdk"
ln -sfn "$SDL2_SDK" "$NAB/vendor/sdl2-real"
for l in libc++.a:libcxx.a libc++abi.a:libcxxabi.a libunwind.a:libunwind.a; do
	ln -sfn "$PAYLOAD_LIB/${l%%:*}" "$NAB/vendor/payload-sdk-cxx/${l##*:}"
done

# libextras.a = LLVM emutls (the OpenGL SDK uses emulated thread-locals)
export PS5_PAYLOAD_SDK="$NAB/.deps/native/ps5-payload-sdk"
CC="sh $NAB/tooling/prospero-clang18"
$CC -O2 -c ps5/build/third_party/emutls.c -o "$NAB/vendor/extras/emutls.c.o"
rm -f "$NAB/vendor/extras/libextras.a" && ar rcs "$NAB/vendor/extras/libextras.a" "$NAB/vendor/extras/emutls.c.o"

make -C ps5/build -j"$(nproc)" NAB="$NAB"
echo "built: $REPO/ps5/build/libred2.a"
