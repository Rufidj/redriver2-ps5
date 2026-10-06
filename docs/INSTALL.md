# Installation guide: Driver 2 on PS5 from scratch

(Versión en español: [INSTALAR.es.md](INSTALAR.es.md).)

This is everything you need, in order. It assumes a PC with Linux and a jailbroken PS5. Nothing here includes the game: you must own
*Driver 2* (PlayStation) and extract its data yourself. A pre-built title is **not** distributed from this repository (it needs
the game's data, and a Sony system module, to run), so you build it.

## 0. Checklist

| You need | Why | Where |
| --- | --- | --- |
| A jailbroken PS5 (tested: firmware 9.00, kstuff 1.13) | to run homebrew titles | your jailbreak of choice |
| ShadowMountPlus payload | makes the console register a title folder in `/data/homebrew` | [drakmor/ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) |
| ftpsrv payload | copies files to the console (port 2121) | [ps5-payload-dev/ftpsrv](https://github.com/ps5-payload-dev/ftpsrv) |
| A Linux PC (WSL may work) on the same network | builds the title | |
| Your own legal copy of *Driver 2* (PlayStation, the discs) | the game data | |
| About 3 GB free on the console, 15 GB on the PC (RADV build) | | |

## 1. Prepare the PC

Install: `git make clang-18 lld-18 python3 python3-numpy python3-pil glslang-tools zip` and a C/C++ toolchain. Then fetch, side by side
in one folder (for example `~/ps5/`):

```sh
git clone https://github.com/mihawk-99/PS5_Vulkan
git clone https://github.com/mihawk-99/PS5_VulkanTemplate
git -C PS5_VulkanTemplate checkout b577e950684e          # the revision this port was made on
git clone https://github.com/blackbearreloaded/ps5-native-app-boilerplate
git clone https://github.com/blackbearreloaded/ps5-opengl
git clone <this repository> redriver2-ps5
```

1. **PS5_Vulkan's RADV**: in `PS5_VulkanTemplate` run `ps5/tools/bootstrap.sh`; it checks the whole stack and builds what is missing
   (RADV is a long build; it lists exactly what the host needs, for example LLVM 18 with its clang libraries, SPIRV-LLVM-Translator, meson).
   Put your console's address in `PS5_Vulkan/.env` (`PS5_HOST=<ip>`).
2. **ps5-native-app-boilerplate**: `make deps` once (downloads the PS5 payload SDK).
3. **ps5-opengl**: build its SDK and its **SDL2 for PS5** (the `native-sdl2-audio` build): this port uses the SDL2 build for the
   PS5 pad and audio back ends (no video) and ps5-opengl's headers.

## 2. Get the game data

Prepare a `DRIVER2/` folder exactly as the desktop version of REDRIVER2 needs it; the upstream wiki explains every step:
[Installation instructions](https://github.com/OpenDriver2/REDRIVER2/wiki/Installation-instructions).

* The files of the game CD (`DATA`, `GFX`, `LEVELS`, `SOUND`, `*.BIN`...), the `LANG` and `REPLAYS` folders of the REDRIVER2 release,
  and the music/voices (`XA`) converted to `.wav` as upstream describes. About 160 MB.
* **Videos** (optional, about 1.3 GB): extract the `.STR` videos of the discs as MJPEG AVI with [jPSXdec](https://github.com/m35/jpsxdec)
  (as upstream's wiki describes); they end up as `DRIVER2/FMV/<n>/RENDER<k>.STR[0].avi`. Without them the game skips the videos.

## 3. Build

```sh
cd redriver2-ps5
cp scripts/env.sh.example scripts/env.sh      # set every path (the checkouts above) and the console IP
./scripts/setup.sh                            # upstream REDRIVER2 + PsyCross at the right commits, patches applied
./scripts/build-vulkan.sh                     # builds the game library, the title and links it with RADV
```

The title is left in `$VK_TITLE_DIR/dist/PPSA00058/` (`eboot.bin`, `sce_sys`, `sce_module`, `shaders`).

> The title folder (`VK_TITLE_DIR`) must sit next to `PS5_Vulkan`, `PS5_Mesa` and `PS5_PayloadSDK` (same parent directory), because the template references them by relative path.

## 4. Optional extras

* **HD sky** (recommended): `python3 tools/sky/prepare_sky.py ~/sky` downloads and converts 18 CC0 panoramas (or take the ZIP of the
  GitHub release). Put the PNGs and `sky.ini` in the title's `assets/DRIVER2/HD/SKY/`.
* **HD textures**: see [tools/hdtex](../tools/hdtex/README.md) (upscaled texture pages, built from your own game's textures).
* **Icon, background and home-screen music**: put `icon0.png` (512x512), `pic0.dds` and `pic1.dds` (3840x2160, BC7) and `snd0.at9`
  (ATRAC9, 48 kHz, at most 2 MiB) into `vulkan/sce_sys/` and run the build again. `scripts/make-snd0.sh song.mp3` makes the music.

## 5. Assemble the title folder

```
PPSA00058/
  eboot.bin  sce_sys/  sce_module/  shaders/  LEGAL.txt  licenses/        <- from the build
  assets/
    config.ini                                                           <- start from ps5/assets/config.ini
    DRIVER2/   (DATA GFX LEVELS ... XA, LANG, REPLAYS, ...)             <- your game data
      FMV/     (the videos, optional)
      HD/      (HD textures, optional)    HD/SKY/  (the sky pack, optional)
```

For the Vulkan title set in `assets/config.ini` (section `[render]`), a good starting point:

```ini
frameInterval=2
hdSky=1
zPrepass=0
farMesh=8
farMeshNear=24
farMeshCull=0
fogStart=100
fogEnd=250
```

## 6. Put it on the console

1. Send the payloads: `nc -q2 <ps5-ip> 9021 < ftpsrv.elf`, then ShadowMountPlus the same way.
2. Upload the folder to `/data/homebrew/PPSA00058`. With PS5_Vulkan's tool: `ps5/tools/deploy.sh --all` in the title folder (its README
   explains the control payload it needs); or with any FTP client (host = the console, port **2121**, no login).
3. Send ShadowMountPlus again so that it registers the title. **Driver 2 Vulkan** appears on the home screen.
4. Start it. Close it before uploading anything again (an open `eboot.bin` cannot be overwritten: `Text file busy`).

## 7. If something goes wrong

| Symptom | What to check |
| --- | --- |
| The title does not appear | ShadowMountPlus was not (re)loaded after the upload; `eboot.bin` and `sce_sys/param.json` must be in the folder |
| Closes at once | read `/data/homebrew/PPSA00058/REDRIVER2.log` (the last lines say where); is `assets/DRIVER2` complete? is the console on a firmware this stack supports? |
| Black screen, no log | RADV / libc.prx missing from `sce_module/`, or the title was built against another PS5_Vulkan revision |
| No videos | `assets/DRIVER2/FMV` is missing or not MJPEG AVI |
| No sound / pad | the SDL2 build must be ps5-opengl's `native-sdl2-audio`; pads need a signed-in user |
| Stripes or missing ground after editing the config | keep `zPrepass=0`, `extendedView` at most 64 |
| Too slow or tearing | `ps5RenderScale=1`; lower `farMesh` (fewer regions); `shadowMapSize=1024` |
| The game runs at double speed | `frameInterval` must be `2` |
| Far field closes the game at the start of a race | note the last lines of the log and open an issue |

## 8. Updating

After a new build, upload again (game closed). `config.ini` is only uploaded when you change it. Remove `farMesh` (or set it to
`0`) to turn the far field off.
