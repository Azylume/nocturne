#!/bin/bash
# On-screen demo of Sections and Cues on a freshly started engine at 120 BPM (a bar is 2 s), about 20 s. Layer 1 steps
# through four counting clips under Autopilot every 2 bars, through a Transform whose scale an Energy macro drives. A
# drop Cue sets Autopilot to every bar and Energy to 1 (the picture zooms in) for as long as the drop lasts. The drop is
# scheduled 2 bars ahead, an outro scheduled behind it is cancelled, and a live breakdown mark ends the drop: Autopilot
# goes back to 2 bars and Energy to 0. Steps and Cues land on the bar's first frame.
#
# usage: demo_cues.sh CTL MEDIA_DIR [PORT [HOST]]
#   CTL        path to nocturne-ctl
#   MEDIA_DIR  absolute path of a folder on the engine's machine with count_709_tv.mp4, count_601_pc.mp4,
#              count_100.mp4 and count_240.mp4
#   PORT       engine port (default 7400), HOST engine host (default 127.0.0.1)
#
# Watch the engine's stats line: 0 video misses while the steps land.
set -eu

CTL=$1
MEDIA=$2
PORT=${3:-7400}
HOST=${4:-127.0.0.1}
BAR=2

run() {
    "$CTL" --host "$HOST" --port "$PORT" "$@"
}

ctl() {
    run "$@" > /dev/null || { echo "failed: $*" >&2; exit 1; }
}

# Runs an adding command and prints the id of what it added (KEY is macro, effect or cue).
add() {
    local key=$1
    shift
    run "$@" | tail -1 | sed -E "s/.*\"$key\":\\{[^}]*\"id\":([0-9]+).*/\\1/"
}

# Prints the Section playing, the Cues holding and the column layer 1 plays.
state() {
    local cue clock
    cue=$(run cue state | tail -1)
    clock=$(run clock | tail -1)
    echo "beat $(echo "$clock" | sed -E 's/.*"beat":([0-9.]+).*/\1/' | cut -c1-6)" \
        "section $(echo "$cue" | sed -E 's/.*"section":("[a-z]+"|null),"sections".*/\1/')" \
        "active $(echo "$cue" | sed -E 's/.*"active":(\[[^]]*\]).*/\1/')" \
        "column $(echo "$clock" | sed -E 's/.*"clips":\[\{[^}]*"column":([0-9]+).*/\1/')"
}

bars() {
    for _ in $(seq "$1"); do
        sleep "$BAR"
        state
    done
}

step() {
    echo "== $*"
}

step "four clips on layer 1, Autopilot every 2 bars, Energy zooms the picture"
ctl bpm 120
ctl cell 1 1 "$MEDIA/count_709_tv.mp4"
ctl cell 1 2 "$MEDIA/count_601_pc.mp4"
ctl cell 1 3 "$MEDIA/count_100.mp4"
ctl cell 1 4 "$MEDIA/count_240.mp4"
effect=$(add effect fx 1 transform)
energy=$(add macro macro add Energy)
ctl macro target "$energy" "1.$effect.scale" 0 0.6
ctl autopilot 1 next 2
ctl cue add Drop drop "autopilot:1=/1" "macro:$energy=1"
ctl column 1
ctl section intro
bars 1

beat=$(run clock | tail -1 | sed -E 's/.*"beat":([0-9.]+).*/\1/')
bar=$(echo "$beat" | awk '{ printf "%d", int($1 / 4) + 3 }')
step "drop scheduled on bar $bar, an outro behind it cancelled"
ctl section drop "$bar"
ctl section outro $((bar + 8))
ctl section cancel $((bar + 8))
run cue state | tail -1
bars 5

step "breakdown on the next bar: Autopilot back to 2 bars, Energy back to 0"
ctl section breakdown
bars 3
echo "done"
