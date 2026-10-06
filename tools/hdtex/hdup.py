#!/usr/bin/env python3
"""Upscales the texture pages dumped by DriverLevelTool with Real-ESRGAN and writes 2x RGBA PNGs.

  hdup.py <textures dir> <output dir> <realesrgan-ncnn-vulkan> [--scale 2]

The dump comes from OpenDriver2Tools' DriverLevelTool (`DriverLevelTool CHICAGO.LEV -textures 1`), which writes
CHICAGO_textures/PAGE_<n>.tga (the level's texture pages with their palettes applied). The game looks for the result in
DRIVER2/HD/<LEVELFILE>/PAGE_<n>.png; the night and multiplayer cities are other LEV files (NLEVELS, MLEVELS, MNLEVELS):
name their dumps NCHICAGO, MCHICAGO, MNCHICAGO... (copy the LEV with that name before running the tool).
"""
import os, sys, subprocess, shutil, tempfile
import numpy as np
from PIL import Image

src, dst, esr = sys.argv[1], sys.argv[2], sys.argv[3]
scale = 2
if '--scale' in sys.argv: scale = int(sys.argv[sys.argv.index('--scale') + 1])
os.makedirs(dst, exist_ok=True)

work = tempfile.mkdtemp(prefix='hdup_')
inp, out = os.path.join(work, 'in'), os.path.join(work, 'out')
os.makedirs(inp); os.makedirs(out)

def dilate_fill(rgb, mask):
    """colours of transparent texels = nearest opaque colour (no dark halos after upscaling)"""
    rgb = rgb.copy()
    known = mask.copy()
    for _ in range(64):
        if known.all(): break
        acc = np.zeros_like(rgb, dtype=np.float32); cnt = np.zeros(mask.shape, dtype=np.float32)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dx == 0 and dy == 0: continue
                k = np.roll(np.roll(known, dy, 0), dx, 1)
                v = np.roll(np.roll(rgb, dy, 0), dx, 1)
                acc += v * k[..., None]; cnt += k
        new = (~known) & (cnt > 0)
        rgb[new] = (acc[new] / cnt[new][:, None]).astype(np.uint8)
        known = known | new
    if not known.all():
        rgb[~known] = rgb[known].mean(axis=0).astype(np.uint8) if known.any() else 0
    return rgb

names = sorted(f for f in os.listdir(src) if f.lower().endswith('.tga'))
alphas = {}
for f in names:
    im = Image.open(os.path.join(src, f)).convert('RGBA')
    a = np.asarray(im)
    mask = a[..., 3] > 127
    alphas[f] = a[..., 3]
    rgb = dilate_fill(a[..., :3], mask) if not mask.all() else a[..., :3]
    Image.fromarray(rgb).save(os.path.join(inp, os.path.splitext(f)[0] + '.png'))

subprocess.run([esr, '-i', inp, '-o', out, '-n', 'realesrgan-x4plus', '-s', '4', '-f', 'png'], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

for f in names:
    base = os.path.splitext(f)[0]
    up = Image.open(os.path.join(out, base + '.png')).convert('RGB')
    size = up.width * scale // 4
    up = up.resize((size, size), Image.LANCZOS)
    a = Image.fromarray(alphas[f]).resize((size, size), Image.BICUBIC)
    a = Image.fromarray(((np.asarray(a) > 127) * 255).astype(np.uint8))
    up.putalpha(a)
    up.save(os.path.join(dst, base + '.png'), optimize=True)
shutil.rmtree(work)
print(len(names), 'pages ->', dst)
