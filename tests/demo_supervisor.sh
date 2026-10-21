#!/bin/bash
# On-screen demo of the supervisor: the engine is killed (SIGKILL) and then hung (SIGSTOP) while Autopilot steps through
# three clips every bar at 120 BPM, and each time it comes back on the same clip and the same beat grid. Layer 1 counts
# up 0..59 per bar in column 1, down in column 2 and up to 99 over 2 bars in column 3, so the bar code shows where in
# the bar the picture is before and after.
#
# usage: demo_supervisor.sh CTL MEDIA_DIR [PORT]
#   CTL        path to nocturne-ctl
#   MEDIA_DIR  absolute path of a folder on this machine with count_709_tv.mp4, count_601_pc.mp4, count_100.mp4
#   PORT       engine port (default 7400)
#
# The engine must run under nocturne-supervisor on this machine, e.g. installed with tools/autostart.sh, or started as
#   nocturne-supervisor -- --port PORT
# STATE overrides the supervisor's state folder (default ~/Library/Application Support/Nocturne/State) and LOGS its
# log folder (default ~/Library/Logs/Nocturne).
set -eu

CTL=$1
MEDIA=$2
PORT=${3:-7400}
STATE=${STATE:-$HOME/Library/Application Support/Nocturne/State}
LOGS=${LOGS:-$HOME/Library/Logs/Nocturne}
BAR=2

ctl() {
    "$CTL" --port "$PORT" "$@" > /dev/null || { echo "failed: $*" >&2; exit 1; }
}

now() {
    perl -MTime::HiRes=time -e 'printf "%.2f\n", time'
}

engine_pid() {
    sed -n 's/.*"engine": \([0-9]*\).*/\1/p' "$STATE/status.json"
}

# How many times an engine has rendered its first frame; the protocol is up by then.
runs() {
    grep -c "engine running" "$LOGS/supervisor.log" || true
}

# The clock state's last line: the beat and layer 1's column and frame.
clock_line() {
    "$CTL" --port "$PORT" clock 2> /dev/null | tail -1
}

show() {
    sed -E 's/.*"beat":([0-9.]+).*"clips":(\[[^]]*\]).*/beat \1  \2/' <<< "$1"
}

beat_of() {
    sed -E 's/.*"beat":([0-9.]+).*/\1/' <<< "$1"
}

bars() {
    for _ in $(seq "$1"); do
        sleep "$BAR"
        show "$(clock_line)"
    done
}

step() {
    echo
    echo "== $*"
}

# Sends SIG to the engine, waits for the supervisor's next engine to play the clip again, and compares the beat with
# where it would be had the engine never stopped.
interrupt() {
    local sig=$1 before at old count line elapsed expected
    before=$(clock_line)
    at=$(now)
    old=$(engine_pid)
    count=$(runs)
    echo "before: $(show "$before")"
    echo "kill -$sig $old"
    kill "-$sig" "$old"
    for _ in $(seq 100); do
        [[ $(runs) -gt $count ]] && break
        sleep 0.1
    done
    for _ in $(seq 100); do
        line=$(clock_line || true)
        [[ "$line" == *'"frame"'* ]] && break
        sleep 0.1
    done
    elapsed=$(awk "BEGIN { printf \"%.1f\", $(now) - $at }")
    expected=$(awk "BEGIN { printf \"%.2f\", $(beat_of "$before") + ($(now) - $at) * 2 }")
    echo "after:  $(show "$line")"
    echo "back on the clip ${elapsed} s after the signal; beat $(beat_of "$line"), expected about $expected"
}

[[ -f "$STATE/status.json" ]] || { echo "no supervisor status in $STATE" >&2; exit 1; }

step "layer 1: three clips, Autopilot next every bar, 120 BPM"
ctl bpm 120
ctl cell 1 1 "$MEDIA/count_709_tv.mp4"
ctl cell 1 2 "$MEDIA/count_601_pc.mp4"
ctl cell 1 3 "$MEDIA/count_100.mp4"
ctl playmode 1 reverse 2
ctl autopilot 1 next 1
ctl column 1
bars 4

step "SIGKILL: a crash"
interrupt KILL
bars 4

step "SIGSTOP: a hang; the supervisor kills it after 2 s without a frame"
interrupt STOP
bars 4

step "supervisor log"
tail -n 12 "$LOGS/supervisor.log"
echo "done"
