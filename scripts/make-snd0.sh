#!/bin/bash
# Builds ps5/app/sce_sys/snd0.at9 (the music the PS5 home screen plays while the title is selected) from any audio file.
#
#   scripts/make-snd0.sh song.mp3 [seconds]
#   scripts/make-snd0.sh song.mp3 [seconds] /path/to/ps4_at9tool.exe     (Sony's encoder instead of tools/at9enc)
#
# The Shell accepts 48 kHz ATRAC9 in RIFF with loop data and at most 2 MiB (<= 192 kb/s stereo): about 87 seconds fit at
# 192 kb/s, a longer song is encoded at a lower rate (give `seconds` to cut an excerpt instead). Needs ffmpeg.
set -e
cd "$(dirname "$0")/.."
IN=${1:?audio file}
SECS=${2:-}
TOOL=${3:-}
OUT=ps5/app/sce_sys/snd0.at9
TMP=$(mktemp -d)
CUT=""; [ -n "$SECS" ] && CUT="-t $SECS"
ffmpeg -hide_banner -loglevel error -y -i "$IN" $CUT -af "loudnorm=I=-16:TP=-1.5:LRA=11,afade=t=in:d=0.5" -ar 48000 -ac 2 -c:a pcm_s16le "$TMP/in.wav"
if [ -n "$TOOL" ]; then
	RUN=""; case "$TOOL" in *.exe) command -v wine >/dev/null && RUN=wine;; esac
	$RUN "$TOOL" -e -br 192 -wholeloop "$TMP/in.wav" "$TMP/out.at9"
else
	tools/at9enc/build.sh
	tools/at9enc/at9enc "$TMP/in.wav" "$TMP/out.at9"
fi
SIZE=$(stat -c %s "$TMP/out.at9")
[ "$SIZE" -le 2097152 ] || { echo "snd0.at9 is $SIZE bytes (> 2 MiB): cut the audio" >&2; exit 1; }
mkdir -p ps5/app/sce_sys && cp "$TMP/out.at9" "$OUT"
echo "wrote $OUT ($SIZE bytes)"
