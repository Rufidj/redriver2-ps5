# REDRIVER2 for PlayStation 5

A port of [REDRIVER2](https://github.com/OpenDriver2/REDRIVER2) (the reverse-engineered *Driver 2*) to jailbroken
PlayStation 5 consoles. It runs the real game code. The main target is a **Vulkan** title on
[mihawk-99](https://github.com/mihawk-99)'s RADV-on-PS5 stack ([PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan),
[PS5_VulkanTemplate](https://github.com/mihawk-99/PS5_VulkanTemplate)); the first version, on the
[ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl) driver, is kept in the repository.

> **This repository contains no game data and no copyrighted assets.** You need your own legal copy of *Driver 2*
> (the PlayStation disc). See [Game data](#game-data).

## What it does

Tested on a PS5 with **firmware 9.00**, kstuff 1.13 and ShadowMountPlus (see [Credits](#credits)). Nothing else has been tried.

| Area | State |
| --- | --- |
| Gameplay, menus, missions, save games, replays, sound, music | working |
| Frame rate | **30 fps** (the game's own pace) at up to 4K internal, with every effect on, the GPU idle and ~8 ms of CPU a frame |
| Real-time sun shadows | three cascades; cars, buildings, street objects, trees and pedestrians cast shadows |
| Real-time lights | street lamps, traffic lights, car head and tail lights (the first cars' lights cast shadows too) |
| Fog, wet roads, bloom, ambient occlusion | working, each switchable in the pause menu |
| HD texture packs | working (optional, see [tools/hdtex](tools/hdtex/README.md)) |
| **HD sky** | a panorama per city and moment of the day (dawn, day, dusk, night, rain), aligned with the game's sun ([tools/sky](tools/sky/README.md)) |
| **Draw distance** | the **whole map** from any position: the area beyond the game's own range is drawn from static meshes ([docs/VULKAN.md](docs/VULKAN.md#the-far-field)) |
| Cut-scene videos (FMV) | MJPEG AVI, uploaded separately (about 1.3 GB) |
| Title artwork, home-screen music | not included (derived from the game); the scripts build them from your own files |
| Installable `.pkg` | not working (error `CE-100096-6` at launch); the title runs from a folder mounted by ShadowMountPlus |

60 fps is possible (`frameInterval=1`) but the game's logic and physics advance one fixed step per frame, so it then runs at
double speed; real 60 fps would need interpolated rendering.

**Full step-by-step guide: [docs/INSTALL.md](docs/INSTALL.md)** (en español: [docs/INSTALAR.es.md](docs/INSTALAR.es.md)).

## Layout

```
patches/            0001 = REDRIVER2 game code, 0002 = PsyCross (the PlayStation layer)
vulkan/overlay/     this port's files for the Vulkan title (renderer, shaders, glue, build helpers)
vulkan/template-changes.patch   the four template files the title changes
ps5/build/          Makefile (game -> libred2.a), software OpenAL, compatibility headers, bundled third-party sources
ps5/app/            the first, OpenGL title (kept for reference)
scripts/            setup.sh, build-game-lib.sh, build-vulkan.sh, build.sh (OpenGL), make-patches.sh, make-snd0.sh
tools/              sky (HD sky pack), hdtex (HD textures), at9enc (ATRAC9 encoder for the home-screen music)
docs/               VULKAN.md (the renderer), SHADOWS.md, NOTES.md (what was learnt about the console)
```

## Requirements

Host (Linux): `git`, `make`, `clang-18`, `python3` with `numpy` and `Pillow` (sky pack), `glslangValidator`, and the build
dependencies of PS5_Vulkan's RADV (see its bootstrap).

Built or downloaded separately:

1. [ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) (run `make deps` once; it
   fetches the PS5 payload SDK).
2. A [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl) SDK (`include/` and `lib/libPS5OpenGL.a`; only its
   headers are used by the Vulkan title) and its SDL2 build (`include/SDL2`, `lib/libSDL2.a`: it brings the PS5 pad and audio
   back ends). Built with SDK 1.0.0 and the `native-sdl2-audio` build.
3. For the Vulkan title: [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) bootstrapped (its RADV archive) and
   [PS5_VulkanTemplate](https://github.com/mihawk-99/PS5_VulkanTemplate) at revision `b577e950684e`, side by side.

Console: a jailbroken PS5 that can launch homebrew titles from `/data/homebrew` through
[ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus), the [ftpsrv](https://github.com/ps5-payload-dev) payload to copy
files (port 2121) and PS5_Vulkan's control payload (`ps5vkctl`) for its deploy tool.

## Build

```sh
git clone <this repository> redriver2-ps5 && cd redriver2-ps5
cp scripts/env.sh.example scripts/env.sh      # edit the paths and the console IP
./scripts/setup.sh                            # clones upstream REDRIVER2 + PsyCross at the right commits and applies the patches
./scripts/build-vulkan.sh                     # libred2.a -> the Vulkan title in $VK_TITLE_DIR/dist/PPSA00058
```

`build-vulkan.sh` builds the game library (`build-game-lib.sh`), has PS5_VulkanTemplate make a title (`new-title.py`), applies
`vulkan/template-changes.patch`, copies `vulkan/overlay/` over it, compiles the shaders and links. The OpenGL title is
`./scripts/build.sh` (see the older notes in `docs/NOTES.md`).

**Artwork and music (optional).** Put `icon0.png` (512x512) and optionally `pic0.dds` / `pic1.dds` (backgrounds, 3840x2160 BC7)
and `snd0.at9` in `vulkan/sce_sys/` and they are packed into the title. They are not included because they derive from the game's
artwork. The music the home screen plays while the title is selected is `snd0.at9` (48 kHz ATRAC9, at most 2 MiB):
`scripts/make-snd0.sh song.mp3` builds it with `tools/at9enc`. The song is yours to supply.

## Game data

Prepare the `DRIVER2/` folder exactly as the desktop version of REDRIVER2 needs it (see the upstream wiki,
[Installation instructions](https://github.com/OpenDriver2/REDRIVER2/wiki/Installation-instructions)): the files of the
game CD (`DATA`, `GFX`, `LEVELS`, `SOUND`, `*.BIN`...), the `LANG` and `REPLAYS` folders of the REDRIVER2 release, and the
music/voices (`XA`) converted to `.wav` as upstream describes. Put it in the title's `assets/DRIVER2/` (about 160 MB without
videos). The cut-scene videos (`DRIVER2/FMV`, MJPEG AVI, about 1.3 GB) are copied on their own. Then add, if you want them,
`DRIVER2/HD` (texture packs) and `DRIVER2/HD/SKY` (the sky pack, see [tools/sky](tools/sky/README.md)); `config.ini` goes next to
`DRIVER2` (start from `ps5/assets/config.ini`).

## Install and run

1. Send ShadowMountPlus (`nc <ps5> 9021 < shadowmountplus.elf`) and the ftpsrv payload.
2. Upload the title folder to `/data/homebrew/PPSA00058` (PS5_Vulkan's `ps5/tools/deploy.sh --all`, or any FTP client).
3. Relaunch ShadowMountPlus so it registers the title, then start **REDRIVER 2** from the home screen.
4. Later builds: `build-vulkan.sh` and `ps5/tools/deploy.sh` in the title folder. The game must be closed first, otherwise the
   upload fails with `Text file busy`.

Logs go to `/data/homebrew/PPSA00058/REDRIVER2.log`; saves and replays to `.../save`.

## Settings

`assets/config.ini` keeps all upstream options and adds these (all optional):

| Key | Default | Meaning |
| --- | --- | --- |
| `[render] frameInterval` | `2` | vblanks per game frame: `2` = 30 fps, `1` = 60 fps at double game speed |
| `[render] farMesh` | `0` (`8` suggested) | regions around you drawn as static far-field meshes, `0` = off |
| `[render] farMeshNear` | `26` | cells around you the game draws itself; the far field covers the rest |
| `[render] farMeshCull` | `1` | back-face culling of the far field: `0` none (recommended), `1` clockwise, `2` counter-clockwise |
| `[render] hdSky` | `1` | HD panorama sky |
| `[render] zPrepass` | `0` | leave at 0 (see docs/VULKAN.md) |
| `[render] ps5RenderScale` | `2` | `1` = native 1080p, `2` = 2x supersampling (3840x2160 internal) |
| `[render] shadowMap`, `shadowMapSize`, `shadowStrength`, `shadowRadius` | `1`, `2048`, `0.5`, `14` | real-time shadows |
| `[render] fog`, `fogStart`, `fogEnd` | `1`, `12`, `30` cells | distance fog; with `farMesh` set them to about `100` and `250` |
| `[render] bloom`, `ssao`, `wetRoads`, `lights`, `lightStrength`, `lightRadius`, `hdTextures` | | effects |
| `[game] extendedView` | `48` | radius in map cells of the game's own drawn area (`21` = original) |
| `[game] farRegions` | `0` | leave at 0 (an old cache, replaced by `farMesh`) |

## Known issues

* The far field draws buildings and ground; **trees and other sprites are not drawn** beyond the game's own range, and the
  far ground uses the game's low-detail tiles.
* Where the game's own drawing ends (`farMeshNear`) the far field takes over with the game's low-detail models.
* No `.pkg` (see above). The `LibProsperoPkg` attempt is described in `docs/NOTES.md`.
* Debug logging and the in-place debugging files (`dumpnow`, `tracenow`) are still in.

## Credits

* [OpenDriver2/REDRIVER2](https://github.com/OpenDriver2/REDRIVER2) (MIT) and its PsyCross layer: the game code. *Driver 2* is
  Reflections / Infogrames / Atari; this port is not affiliated with them.
* [mihawk-99](https://github.com/mihawk-99): PS5_Vulkan, PS5_VulkanTemplate, PS5_Mesa (RADV on the PS5) and the payload SDK fork.
* [drakmor/ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus), kstuff, [ps5-payload-dev](https://github.com/ps5-payload-dev).
* [blackbearreloaded](https://github.com/blackbearreloaded): ps5-opengl and ps5-native-app-boilerplate (the first version, and the SDL2 build).
* [Poly Haven](https://polyhaven.com) (CC0): the panoramas of the HD sky pack.

## License

MIT for this port's files (see [LICENSE](LICENSE)); REDRIVER2 is MIT ([LICENSE-REDRIVER2-MIT](LICENSE-REDRIVER2-MIT)); the sky
panoramas are CC0.
