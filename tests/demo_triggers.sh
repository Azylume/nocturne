#!/bin/bash
# On-screen demo of the clip grid and quantised triggers on a freshly started engine: a 3-layer, 4-column grid of the
# test clips at 128 BPM, column triggers on the bar, clip triggers, a 2-bar quantisation, and a deck switch.
# Layer 1 fills the screen; layers 2 and 3 are half-size in the top-left and bottom-right quadrants.
#
# usage: demo_triggers.sh CTL MEDIA_DIR [PORT [HOST]]
#   CTL        path to nocturne-ctl
#   MEDIA_DIR  absolute path of tests/media on the engine's machine
#   PORT       engine port (default 7400), HOST engine host (default 127.0.0.1)
#
# Watch the engine's stats line: 0 skipped and 0 video misses while the triggers land.
set -eu

CTL=$1
MEDIA=$2
PORT=${3:-7400}
HOST=${4:-127.0.0.1}
BAR=1.875

ctl() {
    "$CTL" --host "$HOST" --port "$PORT" "$@" > /dev/null || { echo "failed: $*" >&2; exit 1; }
}

# Prints each layer's column and frame as of the engine's last frame.
clock() {
    "$CTL" --host "$HOST" --port "$PORT" clock | tail -1 |
        sed -E 's/.*"beat":([0-9.]+).*"clips":(\[[^]]*\]).*/beat \1  \2/'
}

step() {
    echo "== $*"
}

step "grid: 3 layers, 4 columns, 128 BPM, quantise to the bar"
ctl addlayer
ctl addlayer
ctl fx 2 transform scale=0.5 positionX=-0.25 positionY=0.25
ctl fx 3 transform scale=0.5 positionX=0.25 positionY=-0.25
ctl bpm 128
ctl quantise bar
ctl cell 1 1 "$MEDIA/count_240.mp4"
ctl cell 1 2 "$MEDIA/count_100.mp4"
ctl cell 1 3 "$MEDIA/count_601_pc.mp4"
ctl cell 1 4 "$MEDIA/count_709_tv.mp4"
ctl playmode 1 reverse 4
ctl cell 2 1 "$MEDIA/count_709_tv.mp4"
ctl cell 2 2 "$MEDIA/count_240.mp4"
ctl playmode 2 reverse 2
ctl cell 2 4 "$MEDIA/count_100.mp4"
ctl cell 3 1 "$MEDIA/count_100.mp4"
ctl cell 3 2 "$MEDIA/count_601_pc.mp4"
ctl cell 3 3 "$MEDIA/count_240.mp4"

step "deck 2 with columns 5 and 6"
ctl adddeck 2
ctl cell 1 5 "$MEDIA/count_709_tv.mp4"
ctl cell 2 5 "$MEDIA/count_601_pc.mp4"
ctl cell 3 6 "$MEDIA/count_240.mp4"
ctl playmode 3 bounce 6

for column in 1 2 3 4; do
    step "column $column on the next bar (layer 2 clears in column 3, layer 3 in column 4)"
    ctl column "$column"
    sleep "$BAR"
    clock
    sleep "$BAR"
done

step "clip triggers: layer 1 to column 2, layer 3 to column 1"
ctl trigger 1 2
ctl trigger 3 1
sleep "$BAR"
clock
sleep "$BAR"

step "retrigger layer 1: it restarts on the bar"
ctl trigger 1 2
sleep "$BAR"
clock

step "quantise to 2 bars; column 1"
ctl quantise 2bars
ctl column 1
sleep "$BAR"
clock
sleep "$BAR"
clock
sleep "$BAR"

step "deck 2: what plays keeps playing; then column 5 and column 6 on the bar"
ctl quantise bar
ctl deck 2
sleep "$BAR"
clock
ctl column 5
sleep "$BAR"
clock
sleep "$BAR"
ctl column 6
sleep "$BAR"
clock
sleep "$BAR"

step "back to deck 1, column 1"
ctl deck 1
ctl column 1
sleep "$BAR"
clock
echo "done"
