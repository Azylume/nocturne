#!/bin/bash
# The remote part of the chaos rehearsal, run on the Mac mini (tools/chaos-mini.sh starts it there). It runs the soak
# show (bench/soak.sh) with its media on a disk image (a removable drive) and its state on a small disk image (a
# disk that fills up), and while it plays:
#   1. kills client connections: three local watchers and one stalled for 30 s, then killed
#   2. kills the engine (SIGKILL): the supervisor restarts it on the same show
#   3. hangs the engine (SIGSTOP): the supervisor kills and restarts it after 2 s without a frame
#   4. fills the state disk: saves fail with one notice, the show goes on; freed, saving resumes
#   5. detaches the media drive (hdiutil detach -force): layers hold their pictures, clips are flagged missing; the
#      drive comes back at the same place and the clips play again
# Each step is logged to OUT_DIR/events.jsonl with its wall-clock time; the per-second stats carry the same clock, so
# tools/chaos-mini.sh can tell what happened around each event.
#
# usage: chaos.sh BIN_DIR MEDIA_DIR OUT_DIR       DISPLAYS, ENGINE_ARGS, PORT and SETTLE [40] (seconds between steps)
#   as for soak.sh
set -uo pipefail

BIN=$1
MEDIA=$2
OUT=$3
SETTLE=${SETTLE:-40}
PORT=${PORT:-7498}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$OUT/work
DRIVE=$WORK/Show
STATE_DISK=$WORK/StateDisk
SOAK=
WATCHERS=()

event() {
    printf '{"unix":%s,"event":"%s","detail":"%s"}\n' "$(date +%s)" "$1" "${2:-}" >> "$OUT/events.jsonl"
    echo "$(date '+%T') $1 ${2:-}"
}

engine_pid() {
    sed -n 's/.*"engine": *\([0-9]*\).*/\1/p' "$STATE_DISK/status.json" 2> /dev/null | head -1
}

ctl() {
    "$BIN/nocturne-ctl" --port "$PORT" "$@" > /dev/null 2>&1
}

cleanup() {
    for w in ${WATCHERS[@]+"${WATCHERS[@]}"}; do kill -9 "$w" 2> /dev/null; done
    if [[ -n $SOAK ]]; then
        kill -TERM "$SOAK" 2> /dev/null
        wait "$SOAK" 2> /dev/null
    fi
    command rm -f "$STATE_DISK/fill"
    hdiutil detach -force "$DRIVE" > /dev/null 2>&1
    hdiutil detach -force "$STATE_DISK" > /dev/null 2>&1
}

mkdir -p "$WORK" "$DRIVE" "$STATE_DISK"
: > "$OUT/events.jsonl"
trap cleanup EXIT

# A 400 MB "drive" holding the clips, and a 20 MB disk for the state.
hdiutil create -quiet -ov -size 400m -fs HFS+ -volname Show -srcfolder "$MEDIA" "$WORK/show.dmg"
hdiutil attach -quiet -nobrowse -mountpoint "$DRIVE" "$WORK/show.dmg"
hdiutil create -quiet -ov -size 20m -fs HFS+ -volname State "$WORK/state.dmg"
hdiutil attach -quiet -nobrowse -mountpoint "$STATE_DISK" "$WORK/state.dmg"
event setup "media on $DRIVE, state on $STATE_DISK"

MINUTES=60 STATE=$STATE_DISK PORT=$PORT "$HERE/soak.sh" "$BIN" "$DRIVE" "$OUT/show" > "$OUT/show.log" 2>&1 &
SOAK=$!
for _ in $(seq 150); do
    [[ -s $OUT/show/minutes.jsonl ]] && break
    sleep 0.2
done
event started "engine $(engine_pid)"
sleep "$SETTLE"

for _ in 1 2 3; do
    "$BIN/nocturne-ctl" --port "$PORT" watch > /dev/null 2>&1 &
    WATCHERS+=($!)
done
"$BIN/nocturne-ctl" --port "$PORT" watch > /dev/null 2>&1 &
stalled=$!
sleep 5
kill -STOP "$stalled"
event client-stalled "pid $stalled"
for i in $(seq 30); do ctl opacity 5 "0.$((i % 9 + 1))"; sleep 1; done
for w in "${WATCHERS[@]}" "$stalled"; do kill -9 "$w" 2> /dev/null; done
WATCHERS=()
event clients-killed "3 watchers and the stalled one"
sleep "$SETTLE"

old=$(engine_pid)
kill -9 "$old"
event engine-killed "pid $old"
sleep "$SETTLE"
event engine-back "pid $(engine_pid)"

old=$(engine_pid)
kill -STOP "$old"
event engine-hung "pid $old"
sleep "$SETTLE"
event engine-back "pid $(engine_pid)"

# Down to the last block: big writes first, then small ones into what is left.
for bs in 1m 64k 4k 512; do dd if=/dev/zero bs="$bs" >> "$STATE_DISK/fill" 2> /dev/null; done
event disk-full "$(df -k "$STATE_DISK" | awk 'NR == 2 {print $4}') KB free"
for i in $(seq 10); do ctl opacity 5 "0.$((i % 9 + 1))"; sleep 1; done
sleep "$SETTLE"
command rm -f "$STATE_DISK/fill"
event disk-freed
for i in 1 2 3; do ctl opacity 5 "0.$i"; sleep 1; done
sleep 10

hdiutil detach -force "$DRIVE" > /dev/null
event drive-removed
sleep "$SETTLE"
hdiutil attach -quiet -nobrowse -mountpoint "$DRIVE" "$WORK/show.dmg"
event drive-back
sleep "$SETTLE"
event end
grep -hE "cannot save|saved again|gone, flagged|file is back|engine (started|running|killed|crashed|hung|quit)|restarting|safe mode" \
    "$OUT/show/logs/engine.log" "$OUT/show/logs/supervisor.log" 2> /dev/null | sort > "$OUT/notices.txt"
