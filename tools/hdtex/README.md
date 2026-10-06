# HD texture packs

The game replaces each texture page of a level (256x256, 4 bit) with an upscaled RGBA PNG when
`DRIVER2/HD/<LEVELFILE>/PAGE_<n>.png` exists (`hdTextures=1`).

1. **Dump the pages.** Build `DriverLevelTool` from [OpenDriver2Tools](https://github.com/OpenDriver2/OpenDriver2Tools)
   (its viewer, which needs ImGui and SDL, can be left out with a stub `ViewerMain`; `libnstd` at the commit its submodule pins;
   `-include nstd/Math.hpp` and a `stricmp` define for Linux) and run `DriverLevelTool CHICAGO.LEV -textures 1`.
   The night and multiplayer cities are other LEV files: copy `NLEVELS/CHICAGO.LEV` as `NCHICAGO.LEV`
   (`MCHICAGO`, `MNCHICAGO`), so that the dump folder is named like the folder the game reads.
2. **Upscale.** `hdup.py CHICAGO_textures HD/CHICAGO realesrgan-ncnn-vulkan` runs Real-ESRGAN (xinntao, BSD-3-Clause;
   the `realesrgan-x4plus` model of the ncnn-vulkan release) at 4x, brings it back to 2x with Lanczos and rebuilds the
   alpha. About 75 s per city on an RTX 2070 SUPER.
3. **Upload** the `HD` folder to `assets/DRIVER2/` of the title.

The palette variants (`PAGE_1_0.tga`...) are not used. Real-ESRGAN: https://github.com/xinntao/Real-ESRGAN
