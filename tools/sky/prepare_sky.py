#!/usr/bin/env python3
"""Builds the HD sky pack of the PS5 mod: DRIVER2/HD/SKY/<name>.png (4096x2048, equirectangular) and SKY/sky.ini.

The panoramas are Poly Haven's "pure sky" HDRIs (CC0, https://polyhaven.com/hdris/skies): no ground, only sky.
Each one is tone mapped to 8 bit; the tool measures where its sun (or moon) is (azimuth as a fraction of the width,
elevation in degrees) and the colour of its horizon haze (the fog colour). The game turns the picture until that sun
sits where the game's own sun is, so the sky and the shadows agree.

sky.ini maps every city and moment of the day to a panorama: [CHICAGO] day=<name> ...  and [<name>] sun_u= fog= ...

usage: prepare_sky.py OUT_DIR      (downloads the 4k .hdr files into OUT_DIR/_hdr)
"""
import os, sys, urllib.request
import numpy as np
from PIL import Image

# panorama -> exposure (night skies are dark pictures)
EXPOSURE = {
    'night': 0.3,
}
# city -> moment -> Poly Haven id
CITIES = {
    'CHICAGO': {   # grey, industrial, a big city under clouds
        'day': 'evening_road_01_puresky', 'day_rain': 'overcast_soil_puresky', 'dawn': 'kloppenheim_01_puresky',
        'dusk': 'industrial_sunset_puresky', 'night': 'kloppenheim_07_puresky', 'night_rain': 'kloppenheim_07_puresky'},
    'HAVANA': {    # warm, tropical, blue
        'day': 'citrus_orchard_puresky', 'day_rain': 'kloofendal_overcast_puresky', 'dawn': 'qwantani_dawn_puresky',
        'dusk': 'qwantani_dusk_2_puresky', 'night': 'qwantani_moonrise_puresky', 'night_rain': 'kloppenheim_07_puresky'},
    'VEGAS': {     # desert: clear and deep
        'day': 'syferfontein_18d_clear_puresky', 'day_rain': 'kloofendal_overcast_puresky', 'dawn': 'syferfontein_6d_clear_puresky',
        'dusk': 'syferfontein_1d_clear_puresky', 'night': 'kloppenheim_02_puresky', 'night_rain': 'kloppenheim_07_puresky'},
    'RIO': {       # humid, bright, scattered clouds
        'day': 'scythian_tombs_puresky', 'day_rain': 'kloofendal_28d_misty_puresky', 'dawn': 'qwantani_sunrise_puresky',
        'dusk': 'belfast_sunset_puresky', 'night': 'kloppenheim_02_puresky', 'night_rain': 'kloppenheim_07_puresky'},
}
NIGHT_IDS = {'kloppenheim_07_puresky', 'kloppenheim_02_puresky', 'qwantani_moonrise_puresky'}
W, H = 4096, 2048


def read_hdr(path):
    with open(path, 'rb') as f:
        data = f.read()
    pos = 0
    while True:                      # header, up to the empty line
        end = data.index(b'\n', pos)
        line = data[pos:end]
        pos = end + 1
        if line == b'':
            break
    end = data.index(b'\n', pos)
    res = data[pos:end].split()
    pos = end + 1
    h, w = int(res[1]), int(res[3])
    img = np.zeros((h, w, 4), np.uint8)
    for y in range(h):
        if data[pos] != 2 or data[pos + 1] != 2:
            raise SystemExit('flat HDR not supported')
        pos += 4
        for c in range(4):
            x = 0
            row = img[y, :, c]
            while x < w:
                n = data[pos]; pos += 1
                if n > 128:
                    n -= 128
                    row[x:x + n] = data[pos]; pos += 1
                else:
                    row[x:x + n] = np.frombuffer(data, np.uint8, n, pos); pos += n
                x += n
    e = img[:, :, 3].astype(np.int32)
    scale = np.where(e > 0, np.ldexp(1.0, e - 136), 0.0).astype(np.float32)
    return img[:, :, :3].astype(np.float32) * scale[:, :, None]


def tonemap(hdr, exposure):
    x = hdr * exposure
    a, b, c, d, e, f = 0.22, 0.30, 0.10, 0.20, 0.01, 0.30      # a filmic shoulder, then the display gamma
    def curve(v): return ((v * (a * v + c * b) + d * e) / (v * (a * v + b) + d * f)) - e / f
    y = curve(x * 2.0) / curve(np.float32(11.2))
    y = np.clip(y, 0, 1) ** (1 / 2.2)
    return (y * 255 + 0.5).astype(np.uint8)


def main():
    out = sys.argv[1]
    os.makedirs(os.path.join(out, '_hdr'), exist_ok=True)
    ids = sorted({pid for moments in CITIES.values() for pid in moments.values()})
    ini = ['# city -> moment of the day -> panorama; each panorama: sun_u = where its sun is across the picture (0..1),',
           '# sun_elev = its elevation in degrees, fog = the haze colour at the horizon (0..255)', '']
    for city, moments in CITIES.items():
        ini.append('[%s]' % city)
        for m, pid in moments.items():
            ini.append('%s=%s' % (m, pid))
        ini.append('')
    for pid in ids:
        hdr_path = os.path.join(out, '_hdr', pid + '_4k.hdr')
        if not os.path.exists(hdr_path):
            url = 'https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/4k/%s_4k.hdr' % pid
            print('downloading', url)
            urllib.request.urlretrieve(url, hdr_path)
        hdr = read_hdr(hdr_path)
        hh, ww = hdr.shape[:2]
        lum = hdr @ np.array([0.2126, 0.7152, 0.0722], np.float32)
        k = 32
        top = lum[:hh // 2]
        sm = top[:hh // 2 // k * k, :ww // k * k].reshape(-1, k, ww // k, k).mean(axis=(1, 3))
        iy, ix = np.unravel_index(np.argmax(sm), sm.shape)
        sun_u = (ix + 0.5) * k / ww
        sun_elev = 90.0 - (iy + 0.5) * k / hh * 180.0
        night = pid in NIGHT_IDS
        ldr = tonemap(hdr, EXPOSURE['night'] if night else 1.0)
        v0, v1 = int((0.5 - 6 / 180) * hh), int(0.5 * hh)        # horizon haze: 0..6 degrees above the horizon
        band = ldr[v0:v1].reshape(-1, 3).astype(np.float32).mean(axis=0)
        img = Image.fromarray(ldr)
        if (ww, hh) != (W, H):
            img = img.resize((W, H), Image.LANCZOS)
        img.save(os.path.join(out, pid + '.png'), optimize=True)
        print('%-40s sun u=%.3f elev=%5.1f  horizon=%d,%d,%d%s' % (pid, sun_u, sun_elev, *band, '  (night)' if night else ''))
        ini.append('[%s]\nsun_u=%.4f\nsun_elev=%.1f\nfog=%d,%d,%d\nnight=%d\n' % (pid, sun_u, sun_elev, *band, 1 if night else 0))
    open(os.path.join(out, 'sky.ini'), 'w').write('\n'.join(ini))


if __name__ == '__main__':
    main()
