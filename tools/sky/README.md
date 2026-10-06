# HD sky pack

`prepare_sky.py OUT_DIR` downloads Poly Haven's "pure sky" HDRIs (CC0, [polyhaven.com/hdris/skies](https://polyhaven.com/hdris/skies)),
tone maps them and writes, in `OUT_DIR`:

* one `<name>.png` per panorama (4096x2048, equirectangular), and
* `sky.ini`: which panorama each city and moment of the day uses, and for each panorama where its sun is (azimuth across the
  picture, elevation) and the colour of its horizon haze (the fog colour).

Copy both into the title's `assets/DRIVER2/HD/SKY/`. The game turns each panorama until its sun sits where the game's own sun is,
so the sky and the shadows agree. Edit `sky.ini` (or the table at the top of the script) to change which sky a city uses.

Moments: `dawn`, `day`, `dusk`, `night`, `day_rain`, `night_rain`. Cities: `CHICAGO`, `HAVANA`, `VEGAS`, `RIO`.

Requires `python3`, `numpy` and `Pillow`. `scripts/make-sky-pack.sh OUT_DIR` zips the result for a release.
