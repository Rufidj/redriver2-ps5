#!/bin/bash
# Zips the HD sky pack built by tools/sky/prepare_sky.py (PNG panoramas + sky.ini + notice) for a release.
set -e
OUT=${1:?usage: make-sky-pack.sh DIR_WITH_THE_PNGS [ZIP]}
ZIP=${2:-$(pwd)/redriver2-ps5-sky-pack.zip}
TMP=$(mktemp -d)
mkdir -p "$TMP/SKY"
cp "$OUT"/*.png "$OUT/sky.ini" "$TMP/SKY/"
cat > "$TMP/SKY/LICENSE-SKIES.txt" <<'EOT'
The panoramas are tone-mapped versions of HDRIs from Poly Haven (https://polyhaven.com), released under CC0 (public domain):
EOT
grep -h "^\[" "$OUT/sky.ini" | grep -v -e CHICAGO -e HAVANA -e VEGAS -e RIO | tr -d '[]' | sed 's/^/  /' >> "$TMP/SKY/LICENSE-SKIES.txt"
echo "Copy this SKY folder to <title>/assets/DRIVER2/HD/SKY/. Tools: tools/sky/prepare_sky.py in the redriver2-ps5 repository." >> "$TMP/SKY/LICENSE-SKIES.txt"
( cd "$TMP" && zip -qr "$ZIP" SKY )
rm -rf "$TMP"
echo "wrote $ZIP"
