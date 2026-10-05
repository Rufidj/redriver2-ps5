# REDRIVER2 for PlayStation 5

A port of [REDRIVER2](https://github.com/OpenDriver2/REDRIVER2) (the reverse-engineered *Driver 2*) to
jailbroken PlayStation 5 consoles. It runs the real game code on top of the
[ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl) OpenGL 4.6 driver (EGL, no emulation of the console).

> **This repository contains no game data and no copyrighted assets.** You need your own legal copy of *Driver 2*
> (the PlayStation disc). See [Game data](#game-data).

## Status

Tested on a PS5 with **firmware 9.00**, kstuff 1.13 and ShadowMountPlus (see [Credits](#credits)). Nothing else has been tried.

| Area | State |
| --- | --- |
| Gameplay, menus, missions, save games, replays | working |
| Sound, music, voices | working (own software mixer on top of `sceAudioOut`) |
| Intro / cut-scene videos (FMV) | working (MJPEG AVI, uploaded separately) |
| Performance | stable 30 fps (the game's own rate) at 1080p; optional 2x supersampling (internal 4K) |
| Draw distance | far beyond the original (`extendedView`), see [Known issues](#known-issues) |
| Real-time sun shadows | working: three cascades (about 40 cells), cars, buildings, street objects, trees and pedestrians cast shadows |
| Fog | working, hides the pop-in at the edge of the loaded map |
| Installable `.pkg` | not working (error `CE-100096-6` at launch); the title runs from a folder mounted by ShadowMountPlus |

## What the port changes

* `PsyCross` (the PlayStation layer of REDRIVER2) gets an **EGL/OpenGL 4.6 back end for the PS5** and a single "uber" GLSL
  program (the driver crashes when a third program is linked).
* The game renders into its **own framebuffer** and presents with a textured quad: blitting to the window buffer costs
  36–120 ms on this driver.
* A **software OpenAL replacement** (`ps5/build/shim/ps5_al.cpp`) feeds `sceAudioOut`.
* **Draw distance**: the original keeps positions in 16 bits and an ordering table of 8 cells. The port uses 32-bit
  positions where needed, a bigger ordering table, vertex buffer and primitive buffer, and a configurable view radius.
* **Real-time shadows** (shadow map from the sun, PCF), replacing the flat blob shadows. See [docs/SHADOWS.md](docs/SHADOWS.md).
* Memory, file-system and save/replay glue for the PS5 sandbox (`ps5/app/src`).

Everything is in two patches against upstream (`patches/`) plus the PS5 application and build files (`ps5/`).

## Requirements

Host (Linux, WSL should work): `git`, `make`, `clang-18`, `python3`, a C++ toolchain for the boilerplate.

You need, built or downloaded separately:

1. [ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) (run `make deps` once; it
   fetches the PS5 payload SDK).
2. A [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl) SDK (`sdk/` with `include/` and `lib/libPS5OpenGL.a`)
   and its SDL2 build (`include/SDL2`, `lib/libSDL2.a`). The port was built with SDK 1.0.0 and the `native-sdl2-audio`
   build of ps5-opengl.

Console: a jailbroken PS5 that can launch homebrew titles from `/data/homebrew` through
[ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus), and the
[ftpsrv](https://github.com/ps5-payload-dev) payload to copy files (the scripts assume it listens on port 2121).

## Build

```sh
git clone <this repository> redriver2-ps5 && cd redriver2-ps5
cp scripts/env.sh.example scripts/env.sh      # edit the paths and the console IP
./scripts/setup.sh                            # clones upstream REDRIVER2 + PsyCross at the right commits and applies the patches
./scripts/build.sh                            # libred2.a -> linked title in $NAB/dist/PPSA00056
```

`build.sh` only links the symbolic names the boilerplate expects (`$NAB/apps/redriver2`, `$NAB/vendor/...`); it does not
modify anything else in the boilerplate.

Put the title's artwork in `ps5/app/sce_sys/` if you want it: `icon0.png` (512x512) and optionally `pic0.dds` /
`pic1.dds` (backgrounds, 3840x2160 BC7). They are not included because they are derived from the game's artwork.
The music the home screen plays while the title is selected is `snd0.at9` (48 kHz ATRAC9, at most 2 MiB):
`scripts/make-snd0.sh song.mp3` builds it with `tools/at9enc`, a small ATRAC9 encoder written for this (Sony's own
encoder is Windows-only and not redistributable; it can still be used, see the script). The song is yours to supply.

## Game data

Prepare the `DRIVER2/` folder exactly as the desktop version of REDRIVER2 needs it (see the upstream wiki,
[Installation instructions](https://github.com/OpenDriver2/REDRIVER2/wiki/Installation-instructions)): the files of the
game CD (`DATA`, `GFX`, `LEVELS`, `SOUND`, `*.BIN`...), the `LANG` and `REPLAYS` folders of the REDRIVER2 release, and the
music/voices (`XA`) converted to `.wav` as upstream describes. Put it at `ps5/app/assets/DRIVER2/` (about 160 MB without
videos). The cut-scene videos (`DRIVER2/FMV`, MJPEG AVI, about 1.3 GB) are optional and are copied on their own.

`config.ini` (copied from `ps5/assets/config.ini` the first time) lives next to it; see [Settings](#settings).

## Install and run

1. Send a payload that loads ShadowMountPlus (`nc <ps5> 9021 < shadowmountplus.elf`) and the ftpsrv payload.
2. First install, from the repository root:
   ```sh
   . scripts/env.sh
   python3 ps5/build/deploy.py --all --fmv /path/to/DRIVER2/FMV
   ```
   (`--fmv` is optional. `--all` has not been run end-to-end yet: the file-by-file upload logic is the same one used for the author's fast loop, but check the result on your console.)
3. Relaunch ShadowMountPlus so it registers the title, then start **REDRIVER2** from the home screen.
4. Later builds: `./scripts/build.sh && python3 ps5/build/deploy.py [--config]`.
   The game must be closed first, otherwise the upload fails with `Text file busy`.

Logs are written to `/data/homebrew/PPSA00056/REDRIVER2.log` (read it over FTP). Saves and replays go to
`/data/homebrew/PPSA00056/save`.

## Settings

`assets/config.ini` keeps all upstream options and adds these (all optional):

| Key | Default | Meaning |
| --- | --- | --- |
| `[render] ps5RenderScale` | `2` | `1` = native 1080p, `2` = 2x supersampling (3840x2160 internal) |
| `[render] shadowMap` | `1` | real-time shadows on/off |
| `[render] shadowMapSize` | `2048` | resolution of each of the 3 cascades (1024..8192) |
| `[render] shadowStrength` | `0.5` | `0` black shadows, `1` invisible |
| `[render] shadowRadius` | `14` | cells around the camera whose objects cast shadows, even when out of view |
| `[render] fog`, `fogStart`, `fogEnd`, `fogR/G/B` | `1`, `12`, `30` cells, haze colour | distance fog (colour scaled by the game's sky brightness) |
| `[render] shadowFlip`, `shadowDebug` | `0` | debugging: reverse the light, show the shadow-map lookups (`2` = black/white) |
| `[render] pgxpChain` | `1` | vertex-data lookup by screen position through a hash chain (`0` = original linear search) |
| `[game] extendedView` | `48` | radius in map cells of the drawn area (`21` = original) |
| `[game] farRegions` | `0` | extra regions drawn from a cache; **leave at 0**, it produces garbage geometry |
| `[game] shadows2d` | `0` | `1` brings back the original flat blob shadows |

## Known issues

* **Popping** at the edge of the loaded map: the game keeps only a 2x2 block of 32x32-cell regions in memory. A cache of
  far regions (`farRegions`) was tried and produces corrupted geometry after a while; its cause was not found.
  A configurable fog hides most of it.
* Rarely, for a few milliseconds, half of the screen shows only the background.
* Pedestrian and car-wheel shadows are approximations or missing.
* No `.pkg` (see Status). The `LibProsperoPkg` attempt is described in `docs/NOTES.md`.
* Debug logging from the work on shadows is still in the patches.

## Repository layout

```
patches/        0001 = REDRIVER2 game code, 0002 = PsyCross (the PlayStation layer)
ps5/app/        the PS5 title: src (heap, file/save glue, GL stubs), sce_sys/param.json
ps5/build/      Makefile (game -> libred2.a), software OpenAL, compatibility headers, FTP deploy script, bundled third-party sources
ps5/assets/     the default config.ini
scripts/        setup.sh, build.sh, make-patches.sh, make-snd0.sh, env.sh.example
tools/at9enc/   ATRAC9 encoder for the home-screen music (LGPL 2.1+)
docs/           NOTES.md (what was learnt about the console), SHADOWS.md (shadow design)
```

## Credits

This port stands on the work of many people. Please support the upstream projects.

* **REDRIVER2 / OpenDriver2**: SoapyMan (lead reverse engineer and programmer), Fireboyd78, Krishty, someone972, Ben Lincoln,
  Stohrendorf and contributors. <https://github.com/OpenDriver2/REDRIVER2> (MIT). **PsyCross / Psy-X** is part of it, based on
  the HLE emulator of TOMB5 by Gh0stBlade.
* ***Driver 2*** is a game by Reflections Interactive, published by Infogrames/Atari. Game data is not included.
* **ps5-opengl** and **ps5-native-app-boilerplate**: BlackBearReloaded. OpenGL 4.6 for the PS5 built on Mesa and
  [OpenGNM](https://github.com/PS4-OpenGNM) (<https://github.com/blackbearreloaded/ps5-opengl>, GPL-3.0-or-later).
  `ps5/app/src/app_heap.c` is taken from it and keeps its license header.
* **ps5-payload-sdk** (v0.42): John Törnblom and the ps5-payload-dev contributors, the SDK and toolchain pieces the
  boilerplate uses; **ftpsrv**, the FTP payload used to copy files, is from the same organisation.
* **ShadowMountPlus**: drakmor. **kstuff**: sleirsgoevy (version 1.13 as distributed by drakmor). **etaHEN**:
  LightningMods and contributors.
* **SDL2**: Sam Lantinga and the SDL contributors (zlib license). **Mesa**: the Mesa developers.
* **LLVM / Clang**: the LLVM Project. `ps5/build/third_party/emutls.c` is LLVM compiler-rt (Apache-2.0 with LLVM exception).
* **stb_image**: Sean Barrett (public domain / MIT), bundled in `ps5/build/third_party/`.
* **FFmpeg**: Rostislav Pehlivanov wrote its ATRAC9 decoder, whose tables and bitstream description `tools/at9enc` is built on (LGPL 2.1+).
* **LibProsperoPkg** (drakmor), tried for a `.pkg`; not part of the result.
* The attributions above are from memory of each project's public pages; check each upstream repository for its full contributor list.
* PS5 port, shadow system, draw-distance work and glue code: Rufidj, with the help of Claude (Anthropic).

## License

The patches in `patches/` are changes to REDRIVER2 and PsyCross and follow their license, MIT
(`LICENSE-REDRIVER2-MIT`). Files taken from ps5-opengl and the bundled third-party files keep their own licenses, which are
stated in their headers. A linked title that includes GPL-3.0-or-later code (`app_heap.c`) is distributed under those terms.
