#!/bin/bash
# Regenerates tests/golden/hap_*_frame7.png: the HAP fixtures the golden test writes with the engine's encoder, decoded
# by ffmpeg's own HAP decoder (a reference independent of ours) and composited over black as the engine composites a
# layer. Homebrew ffmpeg is a tool for making assets only; nothing links it.
#
# usage: tests/media/make_hap_goldens.sh [BUILD_DIR]      (default build)
set -euo pipefail
cd "$(dirname "$0")/../.."
build=${1:-build}
fixtures=$(mktemp -d)
trap 'rm -rf "$fixtures"' EXIT

NOCTURNE_HAP_FIXTURE_DIR=$fixtures "$build/nocturne_tests" -tc="one frame of each HAP variant*" >/dev/null || true
for name in hap hap_alpha hap_q hap_q_alpha; do
    ffmpeg -v error -y -i "$fixtures/$name.mov" -filter_complex \
        "color=black:size=320x180:rate=30[bg];[0:v]select=eq(n\,7),setpts=PTS-STARTPTS,format=rgba[fg];[bg][fg]overlay=shortest=1:format=auto,format=rgba" \
        -frames:v 1 "tests/golden/hap_${name}_frame7.png"
    echo "tests/golden/hap_${name}_frame7.png"
done
