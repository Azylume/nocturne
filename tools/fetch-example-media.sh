#!/bin/bash
# Fetches the free clips of examples/first-show and saves them under the names show.json expects. Existing files are
# kept, so a rerun only does what is missing. Every download is checked against its published SHA-1.
#
# Sources and licence status, as found on the item pages:
#   serpentine.mp4  archive.org/details/danse-serpentine-par-mme-bob-walter  no licence stated on the item
#   diagonale.mp4   archive.org/details/SymphonieDiagonale                    no licence stated on the item
#   opus3.mp4       archive.org/details/LichtspielOpusIII                      Public Domain Mark 1.0
#   moon.mp4        NASA SVS 14992 on Wikimedia Commons                        category "PD NASA" (public domain)
# The Beeple clips (CC BY, credit "Beeple") are not fetched; see docs/getting-started.md.
#
# usage: tools/fetch-example-media.sh [OUT_DIR]      default examples/first-show; needs curl and ffmpeg
set -euo pipefail

OUT=${1:-$(dirname "$0")/../examples/first-show}
mkdir -p "$OUT"
WORK=$(mktemp -d "$OUT/.fetch.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

fetch() { # url sha1 file
    curl -fsSL --retry 3 -o "$WORK/$3" "$1"
    local got
    got=$(shasum -a 1 "$WORK/$3" | cut -d' ' -f1)
    [ "$got" = "$2" ] || { echo "SHA-1 mismatch for $1: expected $2, got $got" >&2; exit 1; }
}

wanted() { [ ! -e "$OUT/$1" ] || { echo "have $1"; return 1; }; }

archive() { # name sha1 item file
    wanted "$1" || return 0
    fetch "https://archive.org/download/$3/$4" "$2" "$1"
    mv "$WORK/$1" "$OUT/$1"
}

archive serpentine.mp4 504d88181aa80232f58d1d4e48fade80c2f53b0d danse-serpentine-par-mme-bob-walter \
    "Danse%20serpentine%20par%20Mme%20Bob%20Walter.ia.mp4"
archive diagonale.mp4 bfcc4ce1a54a74f1380389b15674039d6369dbdb SymphonieDiagonale "Symphonie%20diagonale.mp4"
archive opus3.mp4 9f34ecd22396a37fa1f4725f449f6331cb8b3fee LichtspielOpusIII "Lichtspiel_opus_III_(1924).mp4"

if wanted moon.mp4; then
    fetch "https://upload.wikimedia.org/wikipedia/commons/b/bb/Lunar_Near_and_Far_Side_Phases_%28SVS14992_-_Waxing_4K%29.webm" \
        a86d9602383f4a2b235e6cb7a0b4f5b1bb33addf moon.webm
    ffmpeg -nostdin -loglevel error -i "$WORK/moon.webm" -c:v libx264 -pix_fmt yuv420p -an "$WORK/moon.mp4"
    mv "$WORK/moon.mp4" "$OUT/moon.mp4"
fi
