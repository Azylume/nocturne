#!/bin/bash
# On-screen demo of advanced mapping on a freshly started engine (default composition: screen 1 with slice 1). Each
# step holds for STEP seconds (default 4):
#   1. two tilted halves with overlapping inputs, then soft edges from the overlap
#   2. a Bezier warp bowing the right half
#   3. a screen mask (a diamond) and an inverted slice mask (an octagon) on the left half
#   4. screen and slice colour
#   5. the composition-space test card on every screen, through all of the above
#   6. guides through the slices and a corner target on the display
#
# usage: demo_mapping.sh CTL DISPLAY [PORT [HOST]]
#   CTL      path to nocturne-ctl
#   DISPLAY  the display id of screen 1's output, for the corner target
set -eu

CTL=$1
DISPLAY_ID=$2
PORT=${3:-7400}
HOST=${4:-127.0.0.1}
STEP=${STEP:-4}

ctl() {
    "$CTL" --host "$HOST" --port "$PORT" "$@" > /dev/null || { echo "failed: $*" >&2; exit 1; }
}

step() {
    echo "== $*"
}

step "two tilted halves"
ctl sliceinput 1 0 0 0 0.55 1
ctl corners 1 0.03 0.12 0.50 0.04 0.50 0.96 0.03 0.88
ctl addslice 1 0 0.45 0 0.55 1
ctl corners 2 0.52 0.04 0.97 0.12 0.97 0.88 0.52 0.96
sleep "$STEP"
step "soft edges from the overlap"
ctl softedge 1 auto
sleep "$STEP"

step "Bezier warp on the right half"
ctl warp 2 bezier
ctl warppoints 2 0 0 0.33 0.12 0.67 0.12 1 0 0 0.33 0.33 0.33 0.67 0.33 1 0.33 0 0.67 0.33 0.67 0.67 0.67 1 0.67 \
    0 1 0.33 0.88 0.67 0.88 1 1
sleep "$STEP"
step "subdivided to 2x2 patches, the shape kept"
ctl warp 2 bezier 2 2
sleep "$STEP"

step "screen mask and inverted slice mask"
ctl mask add 1 0 normal 0.75 0.35 0.85 0.5 0.75 0.65 0.65 0.5
ctl mask add 1 1 invert 0.25 0.1 0.4 0.15 0.5 0.3 0.52 0.5 0.45 0.75 0.3 0.88 0.15 0.8 0.05 0.5
sleep "$STEP"

step "colour: the screen warmer, the left half darker"
ctl screencolor 1 0 0 0.15 0 -0.15
ctl slicecolor 1 -0.3 0.2 0 0 0
sleep "$STEP"

step "test card on every screen"
ctl testcard all on
sleep "$STEP"

step "guides and a corner target"
ctl guides x=0.5 y=0.5 x=0.25
ctl showguides on
for p in "0.03 0.12" "0.50 0.04" "0.97 0.12" "0.97 0.88"; do
    # shellcheck disable=SC2086
    ctl target "$DISPLAY_ID" $p
    sleep 1
done
ctl target "$DISPLAY_ID" off
ctl showguides off
sleep "$STEP"
step "done"
