#!/bin/bash
# The soak test, run on the Mac mini (tools/soak-mini.sh starts it there): at least three outputs with the three
# mappings (bench/run.sh), by default three virtual windows until the Mini has more displays, under the supervisor for MINUTES, with Autopilot on four layers, transitions of every type, beat LFOs
# and an audio modulator, Random Jump on the bottom layer and a column trigger every SWITCH_BARS bars. It writes to
# OUT_DIR:
#   stats.jsonl     the engine's per-second stats (--stats-json)
#   minutes.jsonl   a system sample every 10 s: engine RSS and CPU, supervisor starts and mode, CPU speed limit,
#                   mediaanalysisd and Spotlight CPU
#   logs/, state/   the supervisor's logs and state
#   done            written at the end
# A running autostart agent is stopped for the soak and started again afterwards. At the end the Mini lock is
# released if this soak holds it (its owner starts with "soak").
#
# usage: soak.sh BIN_DIR MEDIA_DIR OUT_DIR
#   MINUTES [30]  DISPLAYS ["window-1 window-2 window-3"]  ENGINE_ARGS ["--virtual 3", plus --offscreen off the Mini;
#   off the Mini --audio-input off is added unless ENGINE_ARGS has --audio-input]
#   PORT [7498]  SEED [27]  DURATION: seconds in place of MINUTES, for short runs
#   BPM [128]  SWITCH_BARS [16]
#   Real displays: DISPLAYS="id1 id2 id3" ENGINE_ARGS=""; one display gets all three mappings on it.
#   STATE: the supervisor's state folder [OUT_DIR/state]
#   For bisecting: JUMP, AUTOPILOT, TRANSITIONS, MODULATORS (1 or 0, default 1) and SAMPLE_SECONDS [10]
#   PREVIEW_CLIP=1: a fifth column holds a clip with Trails and Colorize on layer 1 that never plays, and every
#   EDIT_SECONDS [60] Edge Detection is added to it or removed again (edits.log has the times). PREVIEWS=1 also runs the
#   engine with --clip-previews and keeps that clip's preview watched as a UI's Preview Monitor does (preview.log).
#   bench/preview_soak_compare.py compares two such runs.
set -uo pipefail

BIN=$1
MEDIA=$2
OUT=$3
MINUTES=${MINUTES:-30}
DURATION=${DURATION:-$((MINUTES * 60))}
read -r -a OUTPUTS <<< "${DISPLAYS:-window-1 window-2 window-3}"
PORT=${PORT:-7498}
SEED=${SEED:-27}
BPM=${BPM:-128}
SWITCH_BARS=${SWITCH_BARS:-16}
DEFAULT_ENGINE_ARGS="--virtual 3"
# Off the Mac mini (laptops in use) the engine never shows a window.
[[ $(cd "$BIN" && pwd) == "$HOME/Nocturne/"* ]] || DEFAULT_ENGINE_ARGS+=" --offscreen"
ENGINE_ARGS=${ENGINE_ARGS-$DEFAULT_ENGINE_ARGS}
[[ $(cd "$BIN" && pwd) == "$HOME/Nocturne/"* || " $ENGINE_ARGS " == *" --audio-input "* ]] ||
    ENGINE_ARGS+=" --audio-input off"
((${PREVIEWS:-0})) && ENGINE_ARGS+=" --clip-previews"
STATE=${STATE:-$OUT/state}
AGENT=gui/$(id -u)/ai.azylume.nocturne
PLIST=$HOME/Library/LaunchAgents/ai.azylume.nocturne.plist

mkdir -p "$OUT"
SUPERVISOR=
SWITCHER=
EDITOR=
WATCHER=
agent_was_running=0

log() {
    echo "$(date '+%F %T') $*"
}

ctl() {
    "$BIN/nocturne-ctl" --port "$PORT" "$@" > /dev/null || log "failed: ctl $*"
}

clip() {
    echo "$MEDIA/$1.mp4"
}

stop_autostart() {
    if launchctl print "$AGENT" > /dev/null 2>&1; then
        agent_was_running=1
        launchctl bootout "$AGENT"
        log "autostart agent stopped for the soak"
    fi
}

finish() {
    [[ -n $SWITCHER ]] && kill "$SWITCHER" 2> /dev/null
    [[ -n $EDITOR ]] && kill "$EDITOR" 2> /dev/null
    [[ -n $WATCHER ]] && kill "$WATCHER" 2> /dev/null
    if [[ -n $SUPERVISOR ]]; then
        kill -TERM "$SUPERVISOR" 2> /dev/null
        wait "$SUPERVISOR" 2> /dev/null
    fi
    if ((agent_was_running)); then
        launchctl bootstrap "gui/$(id -u)" "$PLIST" && log "autostart agent started again"
    fi
    date '+%F %T' > "$OUT/done"
    if [[ $(head -1 "$HOME/azl.lock/owner" 2> /dev/null) == soak* ]]; then
        command rm -rf "$HOME/azl.lock"
        log "Mini lock released"
    fi
}

start() {
    # shellcheck disable=SC2086
    "$BIN/nocturne-supervisor" --state "$STATE" --logs "$OUT/logs" -- $ENGINE_ARGS --display "${OUTPUTS[0]}" --port "$PORT" \
        --seed "$SEED" --library "$OUT/library" --presets "$OUT/presets.json" --macro-sets "$OUT/macro-sets.json" \
        --stats-json "$OUT/stats.jsonl" > "$OUT/supervisor.out" 2>&1 &
    SUPERVISOR=$!
    for _ in $(seq 100); do
        "$BIN/nocturne-ctl" --port "$PORT" snapshot > /dev/null 2>&1 && return 0
        sleep 0.2
    done
    log "the engine did not start; see $OUT/logs"
    return 1
}

# The three mappings (bench/run.sh): one keystoned slice, two tilted halves, the four quadrants as cube faces, on
# three outputs, or side by side on one.
mappings() {
    if [ ${#OUTPUTS[@]} -ge 3 ]; then
        ctl corners 1 0.06 0.03 0.96 0 1 1 0 0.94
        ctl addscreen "${OUTPUTS[1]}"
        ctl sliceinput 2 0 0 0 0.5 1
        ctl corners 2 0 0.05 0.5 0 0.5 1 0 0.95
        ctl addslice 2 0 0.5 0 0.5 1
        ctl corners 3 0.5 0 1 0.05 1 0.95 0.5 1
        ctl addscreen "${OUTPUTS[2]}"
        ctl sliceinput 4 0 0 0 0.5 0.5
        ctl corners 4 0.02 0.1 0.48 0.02 0.48 0.48 0.02 0.42
        ctl addslice 3 0 0.5 0 0.5 0.5
        ctl corners 5 0.52 0.02 0.98 0.1 0.98 0.42 0.52 0.48
        ctl addslice 3 0 0 0.5 0.5 0.5
        ctl corners 6 0.02 0.58 0.48 0.52 0.48 0.98 0.02 0.9
        ctl addslice 3 0 0.5 0.5 0.5 0.5
        ctl corners 7 0.52 0.52 0.98 0.58 0.98 0.9 0.52 0.98
        return
    fi
    ctl corners 1 0.03 0.15 0.30 0.05 0.31 0.95 0.02 0.85
    ctl addslice 1 0 0 0 0.5 1
    ctl corners 2 0.36 0.12 0.49 0.06 0.49 0.90 0.36 0.84
    ctl addslice 1 0 0.5 0 0.5 1
    ctl corners 3 0.51 0.06 0.64 0.14 0.64 0.86 0.51 0.92
    ctl addslice 1 0 0 0 0.5 0.5
    ctl corners 4 0.83 0.08 0.98 0.22 0.83 0.36 0.68 0.22
    ctl addslice 1 0 0 0.5 0.5 0.5
    ctl corners 5 0.68 0.22 0.83 0.36 0.83 0.70 0.68 0.56
    ctl addslice 1 0 0.5 0 0.5 0.5
    ctl corners 6 0.83 0.36 0.98 0.22 0.98 0.56 0.83 0.70
    ctl addslice 1 0 0.5 0.5 0.5 0.5
    ctl corners 7 0.68 0.60 0.83 0.74 0.83 0.94 0.68 0.80
}

# Effect ids follow the order they are added in: 1-12.
show() {
    local grid=("jump60_long jump60_long jump60_long jump60_long"
                "a30_short e30_short f30_long b30_long"
                "b30_long a30_short e30_short f30_long"
                "e30_short f30_long b30_long a30_short"
                "f30_long b30_long a30_short e30_short")
    local layer column
    ctl bpm "$BPM"
    ctl quantise bar
    for _ in 2 3 4 5; do ctl addlayer; done
    ctl addgroup
    ctl movelayer 4 6 0
    ctl movelayer 5 6 1
    for layer in 1 2 3 4 5; do
        read -r -a row <<< "${grid[$((layer - 1))]}"
        for column in 1 2 3 4; do ctl cell "$layer" "$column" "$(clip "${row[$((column - 1))]}")"; done
    done
    ((${JUMP:-1})) && for column in 1 2 3 4; do ctl jump 1 8 2 "$column"; done
    ctl fx 1 transform scale=1.08 rotation=4
    ctl fx 1 colorize hue=0.6 saturation=0.7
    ctl fx 2 edgeDetection multiplier=0.6
    ctl fx 2 colorize hue=0.2 saturation=0.5
    ctl fx 2 transform scale=1.02
    ctl fx 3 classicTrails feedback=0.5
    ctl fx 3 transform rotation=-3
    ctl fx 4 trails feedback=0.85
    ctl fx 4 colorize hue=0.8 saturation=0.6
    ctl fx 5 stripper number=12 speed=0.5 feather=0.2
    ctl fx 5 transform scale=1.1
    ctl fx 5 colorize hue=0.4 saturation=0.8
    ctl blend 2 screen
    ctl blend 3 add
    ctl blend 6 screen
    ctl opacity 5 0.8
    ((${AUTOPILOT:-1})) && ctl autopilot 2 next 4
    ((${AUTOPILOT:-1})) && ctl autopilot 3 random 2
    ((${AUTOPILOT:-1})) && ctl autopilot 4 random 8
    ((${AUTOPILOT:-1})) && ctl autopilot 5 next 1
    ((${TRANSITIONS:-1})) && ctl transition 2 alpha 2
    ((${TRANSITIONS:-1})) && ctl transition 3 wipe 1
    ((${TRANSITIONS:-1})) && ctl transition 4 dissolve 4
    ((${TRANSITIONS:-1})) && ctl transition 5 push 0.5
    ((${MODULATORS:-1})) && ctl mod lfo 1.1.rotation sine 8 -10 10
    ((${MODULATORS:-1})) && ctl mod lfo 1.2.hue saw 16 0 0.5
    ((${MODULATORS:-1})) && ctl mod lfo 3.opacity square 2 0.6 1
    ((${MODULATORS:-1})) && ctl mod lfo 5.10.speed ramp 4 0.2 1.5
    ((${MODULATORS:-1})) && ctl mod audio 3.6.feedback level 0 0.4
    mappings
    ctl column 1
}

# The preview clip: column 5 of layer 1, which no column trigger reaches and which has no Autopilot. Its effects get
# ids 13 and 14, so the Edge Detection added every other minute is always 15.
preview_clip() {
    ctl addcolumn 1
    ctl cell 1 5 "$(clip b30_long)"
    ctl clipfx 1 5 trails feedback=0.8
    ctl clipfx 1 5 colorize hue=0.3 saturation=0.8
}

edit_preview_clip() {
    local added=0
    while sleep "${EDIT_SECONDS:-60}"; do
        if ((added)); then ctl fxrm 1 15; else ctl clipfx 1 5 edgeDetection multiplier=0.6; fi
        added=$((1 - added))
        echo "$(date +%s) $added" >> "$OUT/edits.log"
    done
}

switch_columns() {
    local interval column=1
    interval=$(echo "scale=3; 240 / $BPM * $SWITCH_BARS" | bc)
    while sleep "$interval"; do
        column=$((column % 4 + 1))
        ctl column "$column"
    done
}

cpu_of() {
    awk -v pattern="$1" '$2 ~ pattern {s += $1} END {print s + 0}' <<< "$2"
}

# The process's physical footprint in MB, as top shows it (it counts the IOSurfaces of decoded frames, RSS does not).
footprint_mb() {
    top -l 1 -pid "$1" -stats mem 2> /dev/null | tail -1 |
        awk '/^[0-9.]+[KMG]\+?$/ {v = $1 + 0; u = substr($1, length($1) - ($1 ~ /\+$/), 1);
             print (u == "G" ? v * 1024 : u == "K" ? v / 1024 : v)}'
}

sample_minute() {
    local status engine rss cpu procs speed footprint
    status=$(cat "$STATE/status.json" 2> /dev/null || echo '{}')
    engine=$(sed -n 's/.*"engine": *\([0-9]*\).*/\1/p' <<< "$status" | head -1)
    footprint=$(footprint_mb "${engine:-0}")
    rss=$(ps -o rss= -p "${engine:-0}" 2> /dev/null | tr -d ' ')
    cpu=$(ps -o %cpu= -p "${engine:-0}" 2> /dev/null | tr -d ' ')
    procs=$(ps -A -o %cpu=,comm=)
    speed=$(pmset -g therm | awk -F= '/CPU_Speed_Limit/ {gsub(/[ \t]/, "", $2); print $2}')
    printf '{"unix":%s,"engine":%s,"footprintMB":%s,"rssMB":%s,"engineCpu":%s,"starts":%s,"mode":"%s","cpuSpeedLimit":%s,"mediaAnalysisCpu":%s,"spotlightCpu":%s}\n' \
        "$(date +%s)" "${engine:-null}" "${footprint:-null}" \
        "$( [[ -n $rss ]] && echo "scale=1; $rss / 1024" | bc || echo null)" \
        "${cpu:-null}" "$(sed -n 's/.*"starts": *\([0-9]*\).*/\1/p' <<< "$status" | head -1 | grep . || echo null)" \
        "$(sed -n 's/.*"mode": *"\([a-z]*\)".*/\1/p' <<< "$status" | head -1)" "${speed:-null}" \
        "$(cpu_of '/mediaanalysisd$' "$procs")" "$(cpu_of '/(mds|mds_stores|mdworker|mdworker_shared)$' "$procs")" \
        >> "$OUT/minutes.jsonl"
}

trap finish EXIT
trap 'exit 1' INT TERM HUP
log "soak: $MINUTES min on ${OUTPUTS[*]}, port $PORT"
stop_autostart
start || exit 1
show
if ((${PREVIEW_CLIP:-0})); then
    preview_clip
    edit_preview_clip &
    EDITOR=$!
fi
if ((${PREVIEWS:-0})); then
    "$BIN/nocturne-ctl" --port "$PORT" clippreview 1 5 $((DURATION + 60)) > "$OUT/preview.log" 2>&1 &
    WATCHER=$!
fi
switch_columns &
SWITCHER=$!
end=$((SECONDS + DURATION))
while ((SECONDS < end)); do
    sample_minute
    sleep "${SAMPLE_SECONDS:-10}" &
    wait $!
done
sample_minute
log "soak: done"
