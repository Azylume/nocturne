#!/bin/bash
# Builds assets/icon/nocturne.ico (16, 24, 32, 48, 64, 128, 256 px, PNG-compressed entries) for Windows.
# 16 comes from svg/nocturne-small16.svg, 24 and 32 from svg/nocturne-small32.svg, 48 from svg/nocturne.svg, and
# 64, 128 and 256 are the PNGs in png/. Needs rsvg-convert (brew install librsvg) and python3 (standard library only).
# Re-run this after replacing the art, then commit the regenerated .ico.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
command -v rsvg-convert > /dev/null || { echo "need rsvg-convert (brew install librsvg)" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
rsvg-convert -w 16 -h 16 "$here/svg/nocturne-small16.svg" -o "$tmp/16.png"
rsvg-convert -w 24 -h 24 "$here/svg/nocturne-small32.svg" -o "$tmp/24.png"
rsvg-convert -w 32 -h 32 "$here/svg/nocturne-small32.svg" -o "$tmp/32.png"
rsvg-convert -w 48 -h 48 "$here/svg/nocturne.svg" -o "$tmp/48.png"
for size in 64 128 256; do
    cp "$here/png/nocturne-$size.png" "$tmp/$size.png"
done

python3 - "$tmp" "$here/nocturne.ico" <<'PY'
import struct
import sys

sizes = [16, 24, 32, 48, 64, 128, 256]
images = [open(f"{sys.argv[1]}/{size}.png", "rb").read() for size in sizes]
offset = 6 + 16 * len(sizes)
out = struct.pack("<HHH", 0, 1, len(sizes))
for size, data in zip(sizes, images):
    out += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
    offset += len(data)
open(sys.argv[2], "wb").write(out + b"".join(images))
PY
echo "wrote $here/nocturne.ico"
