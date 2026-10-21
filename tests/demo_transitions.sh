#!/bin/bash
# On-screen demo of beat-measured transitions on a freshly started engine at 120 BPM (a beat is 0.5 s, a bar 2 s).
# Layer 1 fills the screen and Autopilot steps it through its three clips every bar, with each transition mode in
# turn; layer 2, a quarter in the top-right, picks a random clip every 2 bars and dissolves over 2 beats. Column 1
# counts up 0..59 per bar, column 2 counts down (reverse), column 3 counts up to 99 over 2 bars, so both clips of a
# transition are visible as two moving bar codes. Screens and slices are left as they are.
#
# usage: demo_transitions.sh CTL MEDIA_DIR [PORT [HOST]]
#   CTL        path to nocturne-ctl
#   MEDIA_DIR  absolute path of a folder on the engine's machine with count_100.mp4, count_709_tv.mp4, count_601_pc.mp4
#   PORT       engine port (default 7400), HOST engine host (default 127.0.0.1)
#
# Watch the engine's stats line: 0 video misses and no compiled pipelines while the transitions run.
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
ctl transition 1 alpha 1
ctl transition 2 dissolve 2
ctl autopilot 1 next 1
ctl autopilot 2 random 2
ctl column 1

step "layer 1: alpha over 1 beat, every bar"
bars 6

step "layer 1: wipe over 1 beat"
ctl transition 1 wipe 1
bars 6

step "layer 1: dissolve over 2 beats"
ctl transition 1 dissolve 2
bars 6

step "layer 1: push over 1 beat"
ctl transition 1 push 1
bars 6

step "layer 1: alpha over a whole bar, so a crossfade is always running"
ctl transition 1 alpha 4
bars 6

step "column 2's clip cuts in (its own length 0); the others keep the layer's 4 beats"
ctl cliptransition 1 2 0
bars 6
ctl cliptransition 1 2 layer

step "layer 1: alpha over 2 beats at 120 BPM, then at 60 BPM, where the same 2 beats take twice as long"
ctl transition 1 alpha 2
bars 4
ctl bpm 60
BAR=4
bars 3
ctl bpm 120
BAR=2

step "manual triggers with push over 2 beats: layer 1 to column 3, then column 1, on the next bar"
ctl autopilot 1 off
ctl transition 1 push 2
ctl trigger 1 3
bars 2
ctl trigger 1 1
bars 2

step "cuts again"
ctl autopilot 2 off
ctl transition 1 cut
ctl transition 2 cut
ctl column 2
bars 2
echo "done"
