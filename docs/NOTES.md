# Notes: what the PS5 taught us

Hard-won facts about running this game on the ps5-opengl stack. None of this is obvious from the documentation.

## Console / driver

* **Only two GLSL programs can be linked.** Linking a third one terminates the process (any shader, any order; not memory,
  threads or asserts). The port has a single "uber" shader selected with `uniform int texMode`, plus the present/FMV shader.
  Shadows reuse the uber shader (a pass number uniform), they do not add a program.
* **The default `malloc` is tiny.** `malloc(2.3 MB)` returned NULL after Mesa initialised. The app links `app_heap.c`
  (the ps5-opengl allocator) with `ps5_heap_config.c` (768 MiB) and wraps `malloc calloc realloc free posix_memalign
  malloc_usable_size` (`APP_WRAP_SYMBOLS`).
* **Relative paths fail.** `fopen("x")` gives `EINVAL`, `chdir` does nothing, `getcwd` hangs or crashes. `__wrap_fopen`
  (in `ps5_glue.cpp`) prefixes `/app0/assets/`; `__wrap_mkdir` is the same for folders. Saves and replays go to
  `/app0/save` (= `/data/homebrew/<TITLE>/save`).
* `/data/redriver2` and the original save path are not writable; `GetGameProfilePath` (Game/C/loadsave.c) probes and falls back.
* **Window-buffer blits are slow**: `glBlitFramebuffer` from/to the window framebuffer takes 36–120 ms. Render to an own
  FBO and draw a quad. This is what turned 12 fps with audio stutter into a steady 30 fps.
* Logs must be flushed on every line: after a crash an unflushed log is empty. A log that ends without `LOG CLOSED` is a crash;
  with it, the game exited normally.
* Do not probe the console with signal handlers or odd GL calls: one such probe caused a kernel panic.
* The vblank thread is required (the game's `VSync` waits on its counter); it sleeps 1 ms instead of spinning.

## Audio

`ps5/build/shim/ps5_al.cpp` is a small OpenAL: static buffers, gain, pitch, pan, loops with loop points, source queues and a
mixer thread writing 48 kHz S16 stereo to `sceAudioOut` (grain 512). A +-2 % elastic rate control keeps streamed queues from
running dry; it plays silence rather than reporting `AL_STOPPED` when a queue empties. No EFX/reverb.
`alcGetString` must return a list ending in a double NUL.

## Video

`VideoPlayer.cpp` decodes the MJPEG AVIs with stb_image and reads them through a `FILE*` wrapper (`ReadAVI.h`); no exceptions.

## Upload loop

* ftpsrv: port 2121, the IP changes between boots. `cwd` into each folder before `STOR`/`MLSD`.
* Replacing `eboot.bin` while the game is running gives `Text file busy`.
* The build chain can upload a **stale** eboot if the library build failed or the library was not copied into the app's
  `libs/`: always check the compiler output first. (`scripts/build.sh` stops on errors and copies the library.)

## PGXP (precise vertices) and the renderer

REDRIVER2 draws through PsyCross, which turns PlayStation primitives into triangles. Vertex positions are kept in floating
point by PGXP: every GTE transform stores a record (view-space position, screen-space values, GTE offsets) in a per-frame list;
a primitive remembers an index into that list and finds its vertices by their 16-bit screen coordinates.

* The index is 24 bits and the list holds 2 M entries on PS5 (16 bits wrapped and made cars vanish).
* `addPrim` only stores an index when a transform happened since the previous primitive; primitives that share
  already-transformed vertices got none and fell back to integer screen coordinates. The port keeps the last index for them.
* The lookup is a hash chain over the screen coordinates (newest entry not later than the primitive), and among several
  candidates the depths closest together are chosen (front and back of a car can share a screen position).
* Subdivided primitives (`TileNxN`, `PlotBuildingModelSubdivNxN`) carry no PGXP data; on PS5 the game draws plain
  tiles/buildings instead.
* `ScreenCoordsToEmulator` overwrites the stored position with `value / screenSize - 0.5` after the lookup.
  Reconstruction of view space: `view = (stored + 0.5) * dispSize * 128`.
* Beyond ~32 cells the GTE's SZ saturates at 65535 and distant vertices collide in the lookup.

## Draw distance

* The map is 512x768 cells = 384 regions of 32x32; only a 2x2 block is in memory (slots by region parity, collision, roads and
  AI depend on that indexing; do not touch).
* Object positions are 16 bits, which wraps at +-16 cells; the ordering table used `Z>>1` over 0x2000 entries (~8 cells).
  Fixed with 32-bit variants (`FrustrumCheck16Near`, 64-bit `Apply_InvCameraMatrix*`), `OTSIZE 0x8800`, a 32 MB primitive buffer,
  a 1 M vertex buffer, 16384 splits, and larger `MAX_DRAWN_*` limits.
* Things that were tried and made it worse: raising `MAX_DRAWN_TILES` to 16384 with `extendedView` 64 (ground and distant
  buildings vanish, garbage appears), searching the exact index first, restricting animated objects to the PVS, zNear/zFar changes.
* `farRegions` (a draw-only cache of neighbouring regions in `Game/C/cell.c`) yields garbage after a while, even when limited to
  regions of the same area. Unfound cause; suspects: object index relocation per parity class, slot reuse, straddling objects.
  Models and textures are per area, so the area boundary always shows a pop.

## Installable package (parked)

LibProsperoPkg (drakmor) builds a Homebrew-type `.pkg` that installs, but launching it fails with `CE-100096-6` on 9.00 with
kstuff 1.13, with or without the `a53_ppr_install` step, so the title is run from a ShadowMountPlus folder instead. Other limits
met: the library fails on ~200 MB+ of data in Homebrew mode (`NAPS u2c next-base value ... exceeds the single-byte field`); its
Kraken encoder is single-threaded and very slow (store media uncompressed). Untried: Application mode, klog from etaHEN, asking
in the fPKG discussion channels.
