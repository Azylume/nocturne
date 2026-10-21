#!/bin/bash
# On-screen demo of Autopilot on a freshly started engine at 120 BPM (a bar is 2 s). Layer 1 fills the screen and steps
# through its three clips every bar; layer 2, a quarter in the top-right, picks a random clip every 2 bars, and its
# column 2 clip holds for 4. In both layers column 1 counts up 0..59 per bar, column 2 counts down (reverse) and
# column 3 counts up to 99 over 2 bars, so every step shows as the bar code jumping on the bar.
#
# usage: demo_autopilot.sh CTL MEDIA_DIR [PORT [HOST]]
#   CTL        path to nocturne-ctl
#   MEDIA_DIR  absolute path of a folder on the engine's machine with count_709_tv.mp4, count_601_pc.mp4, count_100.mp4
#   PORT       engine port (default 7400), HOST engine host (default 127.0.0.1)
#
# Watch the engine's stats line: 0 video misses while the steps land.
set -eu

CTL=$1
MEDIA=$2
PORT=${3:-7400}
HOST=${4:-127.0.0.1}
BAR=2

ctl() {
    "$CTL" --host "$HOST" --port "$PORT" "$@" > /dev/null || { echo "failed: $*" >&2; exit 1; }
}

# Prints the beat and each layer's column, the column Autopilot plays next, and the clip frame.
clock() {
    "$CTL" --host "$HOST" --port "$PORT" clock | tail -1 |
        sed -E 's/.*"beat":([0-9.]+).*"clips":(\[[^]]*\]).*/beat \1  \2/'
}

bars() {
    for _ in $(seq "$1"); do
        sleep "$BAR"
        clock
    done
}

step() {
    echo "== $*"
}

step "two layers, three columns, 120 BPM"
ctl addlayer
ctl fx 2 transform scale=0.5 positionX=0.25 positionY=0.25
ctl bpm 120
for layer in 1 2; do
    ctl cell "$layer" 1 "$MEDIA/count_709_tv.mp4"
    ctl cell "$layer" 2 "$MEDIA/count_601_pc.mp4"
    ctl cell "$layer" 3 "$MEDIA/count_100.mp4"
    ctl playmode "$layer" reverse 2
done
ctl step 2 2 4

step "layer 1: next every bar; layer 2: random every 2 bars (column 2 holds 4); column 1 from the next bar"
ctl autopilot 1 next 1
ctl autopilot 2 random 2
ctl column 1
bars 12

step "layer 1: random every bar"
ctl autopilot 1 random 1
bars 8

step "column 3 on the next bar: both layers restart their steps from it"
ctl column 3
bars 6

step "layer 1 off: it holds its clip"
ctl autopilot 1 off
bars 4

step "layer 1 next every 2 bars, from the bar it is in"
ctl autopilot 1 next 2
bars 8

step "both off"
ctl autopilot 1 off
ctl autopilot 2 off
bars 2
echo "done"
