# Guia de instalacion: Driver 2 en PS5 desde cero

(English version: [INSTALL.md](INSTALL.md).)

Esto es todo lo que necesitas, en orden. Supone un PC con Linux y una PS5 con jailbreak. Aqui no va el juego: tienes que tener tu
propia copia de *Driver 2* (PlayStation) y extraer sus datos tu mismo. Este repositorio **no** distribuye un titulo ya compilado (para
funcionar necesita los datos del juego y un modulo de sistema de Sony), asi que lo compilas tu.

## 0. Lista de lo necesario

| Necesitas | Para que | Donde |
| --- | --- | --- |
| Una PS5 con jailbreak (probado: firmware 9.00, kstuff 1.13) | ejecutar titulos homebrew | tu jailbreak de siempre |
| Payload ShadowMountPlus | hace que la consola registre una carpeta de `/data/homebrew` como titulo | [drakmor/ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) |
| Payload ftpsrv | copiar archivos a la consola (puerto 2121) | [ps5-payload-dev/ftpsrv](https://github.com/ps5-payload-dev/ftpsrv) |
| Un PC Linux (WSL puede valer) en la misma red | compilar el titulo | |
| Tu copia legal de *Driver 2* (PlayStation, los discos) | los datos del juego | |
| Unos 3 GB libres en la consola y 15 GB en el PC (compilar RADV) | | |

## 1. Preparar el PC

Instala: `git make clang-18 lld-18 python3 python3-numpy python3-pil glslang-tools zip` y un compilador de C/C++. Despues descarga, una
junto a otra en una misma carpeta (por ejemplo `~/ps5/`):

```sh
git clone https://github.com/mihawk-99/PS5_Vulkan
git clone https://github.com/mihawk-99/PS5_VulkanTemplate
git -C PS5_VulkanTemplate checkout b577e950684e          # la revision con la que se hizo este port
git clone https://github.com/blackbearreloaded/ps5-native-app-boilerplate
git clone https://github.com/blackbearreloaded/ps5-opengl
git clone <este repositorio> redriver2-ps5
```

1. **RADV de PS5_Vulkan**: en `PS5_VulkanTemplate` ejecuta `ps5/tools/bootstrap.sh`; revisa toda la pila y compila lo que falta
   (RADV tarda mucho; te dice exactamente que necesita el PC, por ejemplo LLVM 18 con sus librerias de clang, SPIRV-LLVM-Translator, meson).
   Pon la direccion de tu consola en `PS5_Vulkan/.env` (`PS5_HOST=<ip>`).
2. **ps5-native-app-boilerplate**: `make deps` una vez (descarga el SDK de payloads de PS5).
3. **ps5-opengl**: compila su SDK y su **SDL2 para PS5** (la compilacion `native-sdl2-audio`): este port usa ese SDL2 para el mando y el
   audio de PS5 (sin video) y las cabeceras de ps5-opengl.

## 2. Datos del juego

Prepara una carpeta `DRIVER2/` igual que la que necesita la version de escritorio de REDRIVER2; la wiki original explica cada paso:
[Installation instructions](https://github.com/OpenDriver2/REDRIVER2/wiki/Installation-instructions).

* Los archivos del CD del juego (`DATA`, `GFX`, `LEVELS`, `SOUND`, `*.BIN`...), las carpetas `LANG` y `REPLAYS` de la release de REDRIVER2 y la
  musica y voces (`XA`) convertidas a `.wav` como explica la wiki. Unos 160 MB.
* **Videos** (opcional, unos 1,3 GB): extrae los videos `.STR` de los discos como AVI MJPEG con [jPSXdec](https://github.com/m35/jpsxdec)
  (como describe la wiki); quedan como `DRIVER2/FMV/<n>/RENDER<k>.STR[0].avi`. Sin ellos el juego se salta los videos.

## 3. Compilar

```sh
cd redriver2-ps5
cp scripts/env.sh.example scripts/env.sh      # pon todas las rutas (las carpetas de arriba) y la IP de la consola
./scripts/setup.sh                            # REDRIVER2 + PsyCross originales en los commits correctos, con los parches aplicados
./scripts/build-vulkan.sh                     # compila la libreria del juego, el titulo y lo enlaza con RADV
```

El titulo queda en `$VK_TITLE_DIR/dist/PPSA00058/` (`eboot.bin`, `sce_sys`, `sce_module`, `shaders`).

> La carpeta del titulo (`VK_TITLE_DIR`) debe estar junto a `PS5_Vulkan`, `PS5_Mesa` y `PS5_PayloadSDK` (mismo directorio padre), porque la plantilla las referencia con rutas relativas.

## 4. Extras opcionales

* **Cielo HD** (recomendado): `python3 tools/sky/prepare_sky.py ~/cielos` descarga y convierte 18 panoramas CC0 (o coge el ZIP de la release de
  GitHub). Pon los PNG y `sky.ini` en `assets/DRIVER2/HD/SKY/` del titulo.
* **Texturas HD**: mira [tools/hdtex](../tools/hdtex/README.md) (paginas de textura ampliadas, hechas con las texturas de tu propio juego).
* **Icono, fondo y musica del menu de la consola**: pon `icon0.png` (512x512), `pic0.dds` y `pic1.dds` (3840x2160, BC7) y `snd0.at9`
  (ATRAC9, 48 kHz, maximo 2 MiB) en `vulkan/sce_sys/` y vuelve a compilar. `scripts/make-snd0.sh cancion.mp3` crea la musica.

## 5. Montar la carpeta del titulo

```
PPSA00058/
  eboot.bin  sce_sys/  sce_module/  shaders/  LEGAL.txt  licenses/        <- de la compilacion
  assets/
    config.ini                                                           <- parte de ps5/assets/config.ini
    DRIVER2/   (DATA GFX LEVELS ... XA, LANG, REPLAYS, ...)             <- los datos de tu juego
      FMV/     (los videos, opcional)
      HD/      (texturas HD, opcional)    HD/SKY/  (el pack de cielos, opcional)
```

En `assets/config.ini` (seccion `[render]`), un buen punto de partida:

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

## 6. Llevarlo a la consola

1. Envia los payloads: `nc -q2 <ip-de-la-ps5> 9021 < ftpsrv.elf` y despues ShadowMountPlus igual.
2. Sube la carpeta a `/data/homebrew/PPSA00058`. Con la herramienta de PS5_Vulkan: `ps5/tools/deploy.sh --all` dentro de la carpeta del titulo
   (su README explica el payload de control que usa); o con cualquier cliente FTP (host = la consola, puerto **2121**, sin usuario).
3. Envia ShadowMountPlus otra vez para que registre el titulo. **REDRIVER 2** aparece en la pantalla de inicio.
4. Arrancalo. Cierralo antes de volver a subir nada (un `eboot.bin` abierto no se puede sobrescribir: `Text file busy`).

## 7. Si algo falla

| Sintoma | Que mirar |
| --- | --- |
| El titulo no aparece | no se (re)cargo ShadowMountPlus tras subirlo; en la carpeta deben estar `eboot.bin` y `sce_sys/param.json` |
| Se cierra al instante | lee `/data/homebrew/PPSA00058/REDRIVER2.log` (las ultimas lineas dicen donde); es completa `assets/DRIVER2`? firmware compatible con esta pila? |
| Pantalla negra y sin log | falta RADV / libc.prx en `sce_module/`, o el titulo se compilo con otra revision de PS5_Vulkan |
| No hay videos | falta `assets/DRIVER2/FMV` o no son AVI MJPEG |
| Sin sonido o mando | el SDL2 debe ser el `native-sdl2-audio` de ps5-opengl; el mando necesita un usuario con sesion iniciada |
| Rayas o suelo que falta tras tocar la configuracion | deja `zPrepass=0` y `extendedView` como mucho 64 |
| Muy lento o con cortes | `ps5RenderScale=1`; baja `farMesh` (menos regiones); `shadowMapSize=1024` |
| El juego va al doble de velocidad | `frameInterval` debe ser `2` |
| El campo lejano cierra el juego al empezar una carrera | anota las ultimas lineas del log y abre un issue |

## 8. Actualizar

Tras una compilacion nueva, vuelve a subir (con el juego cerrado). `config.ini` solo se sube cuando lo cambias. Quita `farMesh` (o ponlo a `0`)
para apagar el campo lejano.

## 9. Idioma

Los textos del juego salen de `DRIVER2/LANG/*_GAME.LTXT` y `*_MISSION.LTXT` de tu copia. El idioma se elige en `assets/config.ini`, seccion
`[game]`: `languageId=0` ingles, `1` italiano, `2` aleman, `3` frances, `4` espanol. Las imagenes de los menus salen de los archivos del juego
(`GFX/`, `FRONTEND.BIN`), asi que usa los de la misma version de idioma.

## 10. 60 fps

La logica del juego va a 30 Hz. El titulo Vulkan dibuja cada paso dos veces, una imagen intermedia (coches y camara interpolados) y la real, y asi salen 60 fps. Los peatones y las particulas siguen a 30 Hz. Se apaga con `interpolate=0` en `assets/config.ini`, seccion `[render]` (viene encendido). Se desactiva solo en pausa, cinematicas, fundidos y con dos jugadores.

## 11. Agua

Todos los niveles menos Las Vegas dibujan bajo el mundo el mar del propio juego (el "plano de mar" al que cae la informacion de superficies): el rio de Chicago, el mar de La Habana y de Rio, el borde del mapa. Es casi transparente, con un fondo de piedra visto a traves. `water=0` en `[render]` lo apaga; `waterReflect=1` ademas refleja el campo lejano (apagado por defecto: los coches, los peatones y lo que dibuja el propio juego no salen en el espejo).
