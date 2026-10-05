# at9enc: a small ATRAC9 encoder

The PS5 home screen plays `sce_sys/snd0.at9` while a title is selected: 48 kHz ATRAC9 in a RIFF container, looped, at
most 2 MiB (<= 192 kb/s stereo). Sony's encoder (`at9tool`) only exists for Windows and is not redistributable, so this
is an independent encoder for exactly that use. It writes the same container as Sony's tool (`fmt`, `fact`, `smpl` loop
chunk, `data`) and an ATRAC9 stream that FFmpeg's decoder reads back correctly (checked: SNR 22 dB at 192 kb/s on music).

```
./build.sh
ffmpeg -i song.mp3 -ar 48000 -ac 2 -c:a pcm_s16le song.wav       # 48 kHz, 16 bit, stereo
./at9enc song.wav snd0.at9 [bytes per superframe]               # default: the largest size (<= 512 = 192 kb/s) that fits 2 MiB
```

A superframe is 1024 samples; 512 bytes = 192 kb/s. A song of up to about 87 s fits at 192 kb/s, a 2 minute one is
encoded at about 140 kb/s (a shorter excerpt sounds better).

## What it does

Stereo with independent channels, 256-coefficient MDCT frames (four per superframe), scale factors coded as VLC
deltas, one spectral-precision offset per frame found by rate control (with a tilt that gives the high bands more
bits), no band extension and no intensity stereo. Quantisation picks, for every group of coefficients, the nearest
symbol of the format's Huffman tables.

It was developed against FFmpeg's ATRAC9 decoder (`libavcodec/atrac9dec.c`). Use an FFmpeg newer than 2024 to check
results: releases up to 6.1 have a wrong entry in the bit-allocation table (`at9_tab_b_dist`, `6` instead of `16`) and
misdecode streams that use the tilt. It has not been verified on a console other than by playing it.

## License

`at9tab_enc.h` is derived from `libavcodec/atrac9tab.h` of FFmpeg (Rostislav Pehlivanov, LGPL 2.1 or later), and the bitstream
syntax follows `atrac9dec.c`. This directory is therefore LGPL 2.1 or later.
