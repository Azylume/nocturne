#!/bin/bash
# Builds assets/icon/nocturne.icns from the PNGs in assets/icon/png with iconutil (Xcode command line tools).
# Re-run this after replacing the PNGs, then commit the regenerated .icns.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
icns="$here/nocturne.icns"

command -v iconutil > /dev/null || { echo "need iconutil (Xcode command line tools)" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
iconset="$tmp/nocturne.iconset"
mkdir "$iconset"
for size in 16 32 128 256 512; do
    cp "$here/png/nocturne-$size.png" "$iconset/icon_${size}x${size}.png"
done
cp "$here/png/nocturne-32.png" "$iconset/icon_16x16@2x.png"
cp "$here/png/nocturne-64.png" "$iconset/icon_32x32@2x.png"
cp "$here/png/nocturne-256.png" "$iconset/icon_128x128@2x.png"
cp "$here/png/nocturne-512.png" "$iconset/icon_256x256@2x.png"
cp "$here/png/nocturne-1024.png" "$iconset/icon_512x512@2x.png"

iconutil -c icns "$iconset" -o "$icns"
echo "wrote $icns"
