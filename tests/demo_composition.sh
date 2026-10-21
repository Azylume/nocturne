#!/bin/bash
# On-screen demo of composition files on a freshly started engine: builds a small show (two layers, effects, Autopilot,
# a transition, a macro and a modulator at 128 BPM), saves it to MEDIA_DIR/demo-show.json, wipes the composition back
# to one empty layer (the test card), then loads the file back and lets it play. Finally undo shows the wiped state
# again for a moment (a load is one undo step) and redo brings the show back.
#
# usage: demo_composition.sh CTL MEDIA_DIR [PORT [HOST]]
#   CTL        path to nocturne-ctl
#   MEDIA_DIR  absolute path of a writable folder on the engine's machine with count_709_tv.mp4, count_601_pc.mp4,
#              count_100.mp4; the show file is written there and its media paths are relative to it
#   PORT       engine port (default 7400), HOST engine host (default 127.0.0.1)
set -eu

CTL=$1
MEDIA=$2
PORT=${3:-7400}
HOST=${4:-127.0.0.1}
SHOW="$MEDIA/demo-show.json"

ctl() {
    "$CTL" --host "$HOST" --port "$PORT" "$@" > /dev/null || { echo "failed: $*" >&2; exit 1; }
}

step() {
    echo "== $*"
}

step "build: two layers, three columns, effects, Autopilot, dissolve, macro, modulator, 128 BPM"
ctl bpm 128
ctl addlayer
ctl fx 1 colorize hue=0.6
ctl fx 2 transform scale=0.5 positionX=0.25 positionY=0.25
for layer in 1 2; do
    ctl cell "$layer" 1 "$MEDIA/count_709_tv.mp4"
    ctl cell "$layer" 2 "$MEDIA/count_601_pc.mp4"
    ctl cell "$layer" 3 "$MEDIA/count_100.mp4"
done
ctl autopilot 1 next 1
ctl autopilot 2 random 2
ctl transition 1 dissolve 2
ctl macro add Hue
ctl macro target 1 1.1.hue 0 0.4
ctl mod lfo 2.opacity sine 4 0.3 1
ctl column 1
sleep 8

step "save to $SHOW"
"$CTL" --host "$HOST" --port "$PORT" save "$SHOW" | tail -1

step "wipe: one empty layer (test card), 120 BPM"
ctl rmlayer 2
ctl rmlayer 1
ctl addlayer
ctl bpm 120
sleep 4

step "load $SHOW: the show is back and plays"
"$CTL" --host "$HOST" --port "$PORT" load "$SHOW" | tail -2
sleep 12

step "undo: the wiped composition for 3 s, then redo"
ctl undo
sleep 3
ctl redo
sleep 6
step "done; the show keeps playing"
