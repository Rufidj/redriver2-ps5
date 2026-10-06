#!/usr/bin/env bash
# Builds game/libred2vk.a: the GL build's REDRIVER2+PsyCross objects without the GL renderer,
# main renamed to red2_main, plus this folder's glue and renderer.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# the repository of the port (the game library libred2.a is built in ps5/build), the upstream sources, and the PS5 stack
REPO=${RED2_REPO:?set RED2_REPO to the redriver2-ps5 checkout}
GL=${RED2_LIBDIR:-$REPO/ps5/build}
GLSRC=${RED2_SRC:-$REPO/REDRIVER2/src_rebuild}
NAB=${NAB:?set NAB to the ps5-native-app-boilerplate checkout (its vendor/ has the SDL2 and ps5-opengl headers)}
SDK=${PS5_PAYLOAD_SDK:?set PS5_PAYLOAD_SDK to the payload SDK of the title}
VULKAN=${PS5_VULKAN_DIR:?set PS5_VULKAN_DIR to the PS5_Vulkan checkout}
CC="sh $VULKAN/tooling/prospero-clang18"
out=$here/build; rm -rf "$out"; mkdir -p "$out/o"
cd "$out/o"
ar x "$GL/libred2.a"
# the GL renderer and its EGL bootstrap are replaced by this folder's renderer
rm -f PsyX_render.cpp.o glad.c.o PsyX_render_ps5.cpp.o
"$SDK/bin/llvm-objcopy" --redefine-sym main=red2_main redriver2_psxpc.cpp.o
CXXFLAGS="-O2 -ffunction-sections -fdata-sections -std=gnu++17 -fno-exceptions -fno-rtti -w -fno-strict-aliasing -include stdint.h -include strings.h -include stdlib.h -include string.h"
# FMV: the OpenGL player, patched to hand each decoded frame to the renderer
INC="-I$GLSRC/utils/video_source -I$GL/shim -I$GLSRC/Game -I$GLSRC/PsyCross/include -I$GLSRC/PsyCross/include/psx -I$GLSRC/PsyCross/src -I$GLSRC -I$NAB/vendor/sdl2-real/include/SDL2 -I$GL/third_party -I$NAB/vendor/ps5-opengl-sdk/include"
DEFS="-DNTSC_VERSION -DNDEBUG -DBUILD_CONFIGURATION_STRING=\"PS5\" -D__PROSPERO__ -DGL_GLEXT_PROTOTYPES=1"
python3 - "$GLSRC/utils/video_source/VideoPlayer.cpp" "$out/VideoPlayer.vk.cpp" <<'PY'
import re, sys
s = open(sys.argv[1]).read()
a = s.index('void FMVPlayerInitGL()')
b = s.index('void FMVPlayerShutdownGL()')
s = s[:a] + 'extern TextureID GR_PS5_FMVCreate();\nextern void GR_PS5_FMVUpload(TextureID, int, int, const u_char*);\nvoid FMVPlayerInitGL()\n{\n\tg_FMVDecodedImageBuffer = (u_char*)malloc(DECODE_BUFFER_ALLOC);\n\tmemset(g_FMVDecodedImageBuffer, 0, DECODE_BUFFER_ALLOC);\n\tg_FMVTexture = GR_PS5_FMVCreate();\n\tg_FMVShader = 1;\n}\n\n' + s[b:]
s = s.replace('\tglUniform1i(g_FMVTextureLoc, 0);\n\tglBindTexture(GL_TEXTURE_2D, g_FMVTexture);\n\tglTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, image_w, image_h, 0, GL_RGB, GL_UNSIGNED_BYTE, g_FMVDecodedImageBuffer);\n', '\tGR_PS5_FMVUpload(g_FMVTexture, image_w, image_h, g_FMVDecodedImageBuffer);\n')
assert 'glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8' not in s
open(sys.argv[2], 'w').write(s)
PY
rm -f "$out/o/VideoPlayer.cpp.o"
CXX17="-O2 -ffunction-sections -fdata-sections -std=gnu++17 -fno-exceptions -fno-rtti -w -Wno-c++11-narrowing -fno-strict-aliasing -include stdint.h -include ps5_compat.h -include strings.h -include stdlib.h -include stdarg.h -include string.h"
PS5_PAYLOAD_SDK=$SDK $CC $CXX17 $DEFS $INC -c "$out/VideoPlayer.vk.cpp" -o "$out/o/VideoPlayer.cpp.o"

# the frame clock: an accurate vblank thread, and a limiter whose interval config.ini sets
python3 "$here/patch_clock.py" "$GLSRC/PsyCross/src/PsyX_main.cpp" "$out/PsyX_main.vk.cpp" "$GLSRC/Game/C/main.c" "$out/main.vk.cpp"
rm -f "$out/o/PsyX_main.cpp.o" "$out/o/main.c.o"
PS5_PAYLOAD_SDK=$SDK $CC $CXX17 $DEFS $INC -c "$out/PsyX_main.vk.cpp" -o "$out/o/PsyX_main.cpp.o"
PS5_PAYLOAD_SDK=$SDK $CC -x c++ $CXX17 $DEFS $INC -iquote $GLSRC/Game/C -iquote $GLSRC/Game/Frontend -c "$out/main.vk.cpp" -o "$out/o/main.c.o"
for f in "$here"/*.cpp; do
  PS5_PAYLOAD_SDK=$SDK $CC $CXXFLAGS -D__PROSPERO__ -c "$f" -o "$(basename "$f").o"
done
rm -f "$here/libred2vk.a"; ar rcs "$here/libred2vk.a" *.o
echo "libred2vk.a: $(ls | wc -l) objects"
