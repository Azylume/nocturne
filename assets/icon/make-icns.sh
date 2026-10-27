#!/bin/bash
# Builds assets/icon/nocturne.icns (and the 1024px nocturne.png) from assets/icon/nocturne.svg.
# Needs rsvg-convert (brew install librsvg) to rasterise the source art and iconutil (Xcode) to pack the .icns.
# Re-run this after editing the SVG, then commit the regenerated .icns and .png.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
svg="$here/nocturne.svg"
png="$here/nocturne.png"
icns="$here/nocturne.icns"

command -v rsvg-convert > /dev/null || { echo "need rsvg-convert (brew install librsvg)" >&2; exit 1; }
command -v iconutil > /dev/null || { echo "need iconutil (Xcode command line tools)" >&2; exit 1; }

rsvg-convert -w 1024 -h 1024 "$svg" -o "$png"

iconset=$(mktemp -d)/nocturne.iconset
mkdir -p "$iconset"
for size in 16 32 128 256 512; do
    rsvg-convert -w "$size" -h "$size" "$svg" -o "$iconset/icon_${size}x${size}.png"
    rsvg-convert -w "$((size * 2))" -h "$((size * 2))" "$svg" -o "$iconset/icon_${size}x${size}@2x.png"
done

iconutil -c icns "$iconset" -o "$icns"
rm -rf "$(dirname "$iconset")"
echo "wrote $icns and $png"
