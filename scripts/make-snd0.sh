#!/bin/bash
# Builds ps5/app/sce_sys/snd0.at9 (the music the PS5 home screen plays while the title is selected) from any audio file.
#
#   scripts/make-snd0.sh song.mp3 /path/to/ps4_at9tool.exe [bitrate]
#
# The ATRAC9 encoder is a Sony tool: this repository neither contains nor downloads it, use a copy you are allowed to use.
# On Linux it runs through wine. The Shell accepts 48 kHz ATRAC9 in RIFF with loop data and at most 2 MiB
# (<= 192 kb/s stereo): 120 kb/s fits about 2 minutes, 192 kb/s about 87 seconds (use a shorter excerpt: -t 80).
set -e
cd "$(dirname "$0")/.."
IN=${1:?audio file}
TOOL=${2:?path to ps4_at9tool.exe}
BR=${3:-120}
OUT=ps5/app/sce_sys/snd0.at9
TMP=$(mktemp -d)
ffmpeg -hide_banner -loglevel error -y -i "$IN" -af "loudnorm=I=-16:TP=-1.5:LRA=11,afade=t=in:d=0.5" -ar 48000 -ac 2 -c:a pcm_s16le "$TMP/in.wav"
RUN=""; case "$TOOL" in *.exe) command -v wine >/dev/null && RUN=wine;; esac
$RUN "$TOOL" -e -br "$BR" -wholeloop "$TMP/in.wav" "$TMP/out.at9"
SIZE=$(stat -c %s "$TMP/out.at9")
[ "$SIZE" -le 2097152 ] || { echo "snd0.at9 is $SIZE bytes (> 2 MiB): lower the bitrate or cut the audio" >&2; exit 1; }
mkdir -p ps5/app/sce_sys && cp "$TMP/out.at9" "$OUT"
echo "wrote $OUT ($SIZE bytes)"
