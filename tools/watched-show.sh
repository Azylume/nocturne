#!/bin/bash
# A watched show session played by a script, for watching it in the laptop UI: it drives the
# show running on the Mac mini (tools/show-to-mini.sh first) with nocturne-ctl, records it, and lands the chaos events
# that need no hands in the middle of it. Every action is printed with its time and logged.
#   warm-up    clip previews on, recording on, every deck in turn with its columns, tempo (bpm, tap, nudge, resync),
#              opacity rides, solo and bypass, Autopilot, a held clip preview
#   chaos      the laptop UI killed and reopened, the engine killed (SIGKILL by PID), the engine hung (SIGSTOP),
#              a client stalled; each followed by playing on until the picture is back
#   wind-down  recording stopped and copied to the laptop, the engine's stats over the session judged per event
#              (bench/watched_report.py) into bench/results/watched-<date>.json
# The events that need hands (display unplug, projector, audio interface, network, power) are listed as skipped.
#
# usage: tools/watched-show.sh [--no-ui]     --no-ui: do not open or kill the laptop UI
# The ctl and the laptop UI reach the engine through an SSH tunnel on UI_PORT (7401), so they need no Local Network
# grant.
#   MINI=user@host or a host name (default: tools/mini-host.sh), PORT (7400), STEP seconds between warm-up actions (8),
#   SETTLE seconds of play after each chaos event (30), DECKS (1 2 3 4), COLUMNS per deck (4)
set -uo pipefail

. "$(dirname "$0")/mini-host.sh"
PORT=${PORT:-7400}
UI_PORT=${UI_PORT:-7401}
STEP=${STEP:-8}
SETTLE=${SETTLE:-30}
DECKS=${DECKS:-1 2 3 4}
COLUMNS=${COLUMNS:-4}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CTL=$ROOT/build/Nocturne.app/Contents/MacOS/nocturne-ctl
UI_APP="$ROOT/build/Nocturne UI.app"
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=10 "$MINI")
ID=$(date +%Y%m%d-%H%M)
LOG=$ROOT/bench/results/watched-$ID.log
EVENTS=$ROOT/bench/results/watched-$ID.events.jsonl
ui=1
[[ ${1:-} == --no-ui ]] && ui=

say() {
    printf '%s  %s\n' "$(date +%T)" "$*" | tee -a "$LOG"
}

event() {
    printf '{"unix": %s, "event": "%s", "detail": "%s"}\n' "$(date +%s)" "$1" "${2:-}" >> "$EVENTS"
    say "EVENT $1 ${2:-}"
}

ctl() {
    "$CTL" --host 127.0.0.1 --port "$UI_PORT" "$@" 2>&1 | grep -v '"composition":' >> "$LOG"
}

engine_pid() {
    "${SSH[@]}" 'pgrep -x nocturne | head -1'
}

wait_engine() {
    local start=$SECONDS
    until "$CTL" --host 127.0.0.1 --port "$UI_PORT" clock > /dev/null 2>&1; do
        ((SECONDS - start > 60)) && { say "  engine not back after 60 s"; return 1; }
        sleep 0.5
    done
    say "  engine answers again after $((SECONDS - start)) s"
}

open_ui() {
    [[ -n $ui ]] || return 0
    open -n "$UI_APP" --args --host 127.0.0.1 --port "$UI_PORT" --jpeg-previews
    sleep 2
    UI_PID=$(pgrep -n -f "Nocturne UI.app/Contents/MacOS")
    say "  laptop UI open (pid ${UI_PID:-?})"
}

bar() {
    sleep "$(echo "240 / ${BPM:-120}" | bc -l)"
}

warm_up() {
    event warm-up-start
    ctl clippreviews on
    ctl record start codec=hevc
    for deck in $DECKS; do
        ctl deck "$deck"
        say "deck $deck"
        local first=$(((deck - 1) * 12 + 1))
        for ((c = first; c < first + COLUMNS; c++)); do
            ctl column "$c"
            say "  column $c"
            sleep "$STEP"
        done
    done
    say "tempo: 128, tap back to 120, nudge, resync"
    ctl bpm 128
    sleep "$STEP"
    ctl tap 8 0.5
    sleep "$STEP"
    ctl bpm "${BPM:-120}"
    ctl nudge 5
    bar
    ctl nudge 0
    ctl resync
    sleep "$STEP"
    say "opacity rides on layers 2 and 4"
    for v in 0.3 0.6 1 0.7 1 0.4 0; do
        ctl opacity 2 "$v"
        ctl opacity 4 "$v"
        sleep 1
    done
    say "solo layer 3, then bypass layer 4"
    ctl solo 3 on
    sleep "$STEP"
    ctl solo 3 off
    ctl bypass 4 on
    sleep "$STEP"
    ctl bypass 4 off
    say "Autopilot random on layer 1 for a while"
    ctl autopilot 1 random 1
    sleep $((STEP * 3))
    ctl autopilot 1 off
    say "a held clip preview (layer 3, column 2) while the show plays"
    "$CTL" --host 127.0.0.1 --port "$UI_PORT" clippreview 3 2 20 >> "$LOG" 2>&1
    ctl record state
    event warm-up-end
}

kill_ui() {
    [[ -n $ui && -n ${UI_PID:-} ]] || { say "skipped: UI kill (no laptop UI)"; return; }
    event ui-killed "pid $UI_PID"
    kill -9 "$UI_PID"
    sleep 10
    open_ui
    event ui-back
    sleep "$SETTLE"
}

kill_engine() {
    local pid
    pid=$(engine_pid)
    event engine-killed "pid $pid"
    "${SSH[@]}" "kill -9 $pid"
    wait_engine
    event engine-back "pid $(engine_pid)"
    sleep "$SETTLE"
}

hang_engine() {
    local pid
    pid=$(engine_pid)
    event engine-hung "pid $pid"
    "${SSH[@]}" "kill -STOP $pid"
    sleep 3
    wait_engine
    event engine-back "pid $(engine_pid)"
    sleep "$SETTLE"
}

stall_client() {
    "$CTL" --host 127.0.0.1 --port "$UI_PORT" watch > /dev/null 2>&1 &
    local pid=$!
    sleep 2
    event client-stalled "pid $pid"
    kill -STOP "$pid"
    sleep "$SETTLE"
    kill -9 "$pid"
    event client-killed
    sleep 5
}

chaos() {
    event chaos-start
    kill_ui
    ctl column 2
    stall_client
    ctl deck 2
    ctl column 14
    kill_engine
    ctl column 15
    hang_engine
    ctl deck 1
    ctl column 3
    for skipped in display-unplug projector-power audio-interface network-off power-cut media-drive; do
        say "skipped (needs hands or a media disk image): $skipped"
    done
    event chaos-end
}

wind_down() {
    local movie
    event wind-down-start
    ctl record stop
    sleep 3
    movie=$("$CTL" --host 127.0.0.1 --port "$UI_PORT" record state 2>/dev/null | grep -oE '/[^ "]+\.mov' | tail -1)
    if [[ -n $movie ]]; then
        scp -q "$MINI:$movie" "$ROOT/bench/results/watched-$ID.mov" && say "recording: bench/results/watched-$ID.mov"
    fi
    "${SSH[@]}" 'tail -n 20000 ~/Library/Logs/Nocturne/stats.jsonl' > "$ROOT/bench/results/watched-$ID.stats.jsonl"
    "$ROOT/bench/watched_report.py" "$ROOT/bench/results/watched-$ID.stats.jsonl" "$EVENTS" \
        > "$ROOT/bench/results/watched-$ID.json" && say "summary: bench/results/watched-$ID.json"
    event wind-down-end
}

say "watched show on $MINI ($ID), log $LOG"
if nc -z 127.0.0.1 "$UI_PORT" 2> /dev/null; then
    say "  reusing the tunnel on port $UI_PORT"
else
    ssh -o BatchMode=yes -f -N -L "$UI_PORT:127.0.0.1:$PORT" "$MINI"
    say "  tunnel on port $UI_PORT; it stays up for the laptop UI: pkill -f 'ssh.*-L $UI_PORT:' closes it"
fi
wait_engine || exit 1
BPM=$("$CTL" --host 127.0.0.1 --port "$UI_PORT" clock 2>/dev/null | grep -oE '"bpm":[0-9.]+' | head -1 | cut -d: -f2)
open_ui
warm_up
chaos
wind_down
say "done"
