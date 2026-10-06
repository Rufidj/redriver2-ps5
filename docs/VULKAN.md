# The Vulkan back end

The title runs REDRIVER2's own game code on top of **RADV** (Mesa's AMD Vulkan driver) with a PlayStation 5 window
system, using [mihawk-99](https://github.com/mihawk-99)'s `PS5_Vulkan` / `PS5_VulkanTemplate` stack. The OpenGL version
of this port was limited by the driver's per-draw CPU cost (about 190 µs per small draw); the Vulkan one is not: with every
effect on it holds 30 fps with the GPU idle (waiting 0.02 ms for it) and about 8 ms of CPU per frame.

## How the pieces fit

```
REDRIVER2 + PsyCross (patches/)  ->  libred2.a   (ps5/build: the game, its PlayStation layer, a software OpenAL)
      |  game/mklib.sh: drops PsyCross's OpenGL renderer, renames main -> red2_main, patches the FMV player and the
      |  frame clock, adds game/glue.cpp (files, replays)
      v
libred2vk.a  +  libSDL2.a (ps5-opengl's build: PS5 pad + audio backends, no video)
      +  ps5/src/red2_render.cpp   <- the GR_* layer of PsyCross implemented on Vulkan
      +  examples/driver2vulkan    <- a VulkanExampleBase whose render loop is the game's main()
      v
eboot.bin (the template's link recipe with RADV)
```

`vulkan/overlay/` holds this port's files; `vulkan/template-changes.patch` the four template files it changes
(`main.cpp`, `CMakeLists.txt`, `link-title.sh`, `base/vulkanexamplebase.h`).

## The renderer (`ps5/src/red2_render.cpp`)

The game draws the PlayStation's way: a 1024x512 16-bit VRAM, a 256x256 colour lookup, and streams of `GrVertex`
triangles with state set between draws (texture format, blend, depth, stencil, clip).

* **VRAM** is one `R8G8` image changed in place from the CPU array, uploading only the rectangle that changed, in the
  command stream where the game asked (so draws before the write keep the old picture). The sampler **must repeat**:
  texture-page values carry mode bits, the vertex shader's page row runs past 512 and OpenGL's default wrapped it;
  clamping gave vertical stripes.
* **Frame**: the game renders into its own target (`ps5RenderScale` times the window) with its own depth/stencil; a filtered
  full-screen pass with bloom and ambient occlusion presents it. Vertices go through a ring buffer, the matrices and all
  effect settings through a dynamic uniform buffer (a new slot whenever anything changes between draws).
* **Projection**: the game's matrices are OpenGL's. The viewport is flipped (negative height) and clip z is remapped
  to 0..1 in the vertex shader. Pipelines are built the first time a state combination is used.
* **Effects** (ported from the OpenGL back end): sun shadow cascades and headlight shadow maps (depth-only passes into
  array layers), real lights with a normal from screen-space derivatives, wet roads, fog, HD texture pages (a 32-layer
  512x512 array with mip chain, with "holes" where the game rewrites VRAM at run time), bloom, SSAO.
* **FMV**: `VideoPlayer.cpp` is patched at build time to hand each decoded frame to the renderer.
* **StoreFrameBuffer**: the game keeps a copy of the screen in VRAM (pause background, fades): the frame is blitted small,
  read back two frames later and converted to 16 bit.

### Things that went wrong, so they do not again

* A **z prepass** (depth first, then colour with depth EQUAL) made the nearest ground, the cars and the menus vanish:
  the two pipelines do not give bit-identical depth near the camera. It is off for good.
* The game's **frame limiter** sleeps in `SDL_Delay(1)` steps, which made 57 Hz out of 60 (28.4 fps). The vblank thread
  now works on absolute deadlines (`vulkan/overlay/game/patch_clock.py`).
* Raising `MAX_DRAWN_TILES` (more than 2048) breaks the ground and produces garbage: the PlayStation pipeline cannot
  go past about 30 cells (see the far field below).
* `DrawSkyDome` must leave the GTE set to the camera matrix (the code after it depends on it) even when the sky is
  replaced.

## HD sky

`DRIVER2/HD/SKY` holds equirectangular panoramas and `sky.ini`, which maps each city and moment of the day (dawn, day,
dusk, night, and rain/night rain) to one. The game calls `GR_PS5_SkyFrame` from `DrawSkyDome` (it then skips the PSX dome and
the sun/moon/flare sprites); the renderer draws a full-screen triangle, rebuilding each pixel's view ray from the game's
own projection (the GTE distance and the 3D projection), turned so that the panorama's sun lands where the game's sun (the
shadows' direction) is. The haze takes the panorama's horizon colour. `tools/sky/prepare_sky.py` builds the pack from
Poly Haven's CC0 "pure sky" HDRIs.

## The far field

The PlayStation pipeline reaches only about 30 cells: the GTE saturates depth at 16 bits, and the game walks and
transforms every polygon on the CPU each frame (a radius of 110 cells cost 70 ms). Beyond that range the map is drawn
from **static meshes**:

* `Game/C/farmesh.c` bakes, one region per frame, every 8x8-cell chunk into world-space triangles (the building quads the
  game plots, with the game's LOD models, and the ground tiles; trees and other sprites are not drawn).
* It does not depend on what is loaded: the models of every area are copied out of the level's spool lump (which the PC port
  keeps in RAM) and every texture page goes into the renderer's own page store (256x256 palette indices plus palettes), so
  the whole map can be drawn from any position, in any area.
* `far.vert` projects as the GTE and the PGXP vertex shader do, so the far field meets the game's own drawing seamlessly;
  the lighting tables are taken from the game each frame.
* Config: `farMesh` (radius in regions), `farMeshNear` (cells the game draws itself), `farMeshCull`.

## Debugging in place

With the game running, upload (by FTP) an empty file to `/data/homebrew/PPSA00058/`:
`dumpnow` makes the next frame save `dump_frame_N.ppm` and `dump_vram_N.ppm`; `tracenow` logs the draws of the next
frame. `REDRIVER2.log` there reports the frame time, the CPU split (logic, build + draw, shadow pass), the limits in
use and, with `farMesh`, the far field's size.
