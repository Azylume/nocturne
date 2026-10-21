#!/bin/bash
# Runs the benchmark scenarios on this machine and writes raw per-second samples to OUT_DIR: for each run the engine's
# stats (stats.jsonl, measured window only) and system samples (sys.jsonl: GPU utilisation, WindowServer and engine CPU,
# CPU speed limit). Each run starts a fresh engine with its own media library, deleted afterwards.
#
# usage: run.sh BIN_DIR MEDIA_DIR OUT_DIR [typical] [heavy] [stress]   (default typical stress)
#   BIN_DIR holds nocturne and nocturne-ctl; on the Mini ~/Nocturne/Nocturne.app/Contents/MacOS (tools/deploy-mini.sh)
# environment (defaults in brackets):
#   DISPLAYS       the output displays, three or one ["window-1 window-2 window-3"]
#   ENGINE_ARGS    extra engine options ["--virtual 3", plus --offscreen unless BIN_DIR is under ~/Nocturne, as on the
#                  Mini, so a local run shows no window]; use "" with real display ids in DISPLAYS, or add debug options.
#                  Off the Mini --audio-input off is added unless ENGINE_ARGS has --audio-input, so a laptop run never
#                  asks for the microphone
#   JUMP           typical and stress: bottom layer Random Jump interval and distance in beats ["8 2"]
#   HEAVY_JUMP     heavy: the same ["1 8"]
#   RECORD         typical and heavy: record the composition with hevc or prores while measuring [off]
#   SEED [1]  PORT [7499]  BPM [128]
#   WARMUP [15] DURATION [75] SWITCH_BARS [4]     typical, heavy: seconds before measuring, seconds measured, bars
#                                                 per column switch
#   LEVELS ["2x3 3x3 4x3 5x3 6x3 8x3"] LEVEL_WARMUP [8] LEVEL_SECONDS [20]   stress: layers x effects per level
set -euo pipefail

BIN=$1
MEDIA=$2
OUT=$3
shift 3
SCENARIOS=${*:-typical stress}

read -r -a OUTPUTS <<< "${DISPLAYS:-window-1 window-2 window-3}"
DEFAULT_ENGINE_ARGS="--virtual 3"
[[ $(cd "$BIN" && pwd) == "$HOME/Nocturne/"* ]] || DEFAULT_ENGINE_ARGS+=" --offscreen"
ENGINE_ARGS=${ENGINE_ARGS-$DEFAULT_ENGINE_ARGS}
[[ $(cd "$BIN" && pwd) == "$HOME/Nocturne/"* || " $ENGINE_ARGS " == *" --audio-input "* ]] ||
    ENGINE_ARGS+=" --audio-input off"
RECORD=${RECORD:-}
SEED=${SEED:-1}
PORT=${PORT:-7499}
BPM=${BPM:-128}
WARMUP=${WARMUP:-15}
DURATION=${DURATION:-75}
SWITCH_BARS=${SWITCH_BARS:-4}
JUMP=${JUMP:-8 2}
HEAVY_JUMP=${HEAVY_JUMP:-1 8}
LEVELS=${LEVELS:-2x3 3x3 4x3 5x3 6x3 8x3}
LEVEL_WARMUP=${LEVEL_WARMUP:-8}
LEVEL_SECONDS=${LEVEL_SECONDS:-20}

WORK="$OUT/work"
CLIPS=(jump60_long a30_short e30_short b30_long f30_long)
EFFECTS=("transform scale=1.08 rotation=4" "colorize hue=0.6 saturation=0.7" "edgeDetection multiplier=0.6"
         "classicTrails feedback=0.5" "trails feedback=0.85" "stripper number=12 speed=0.5 feather=0.2")
ENGINE_PID=

ctl() {
    "$BIN/nocturne-ctl" --port "$PORT" "$@" > /dev/null || { echo "failed: ctl $*" >&2; return 1; }
}

clip() {
    echo "$MEDIA/$1.mp4"
}

bar_seconds() {
    echo "scale=4; 240 / $BPM" | bc
}

start_engine() {
    local dir=$1
    mkdir -p "$dir"
    : > "$dir/all.jsonl"
    command rm -rf "$WORK"
    mkdir -p "$WORK"
    # shellcheck disable=SC2086
    "$BIN/nocturne" $ENGINE_ARGS --display "${OUTPUTS[0]}" --port "$PORT" --seed "$SEED" --library "$WORK/library" \
        --presets "$WORK/presets.json" --stats-json "$dir/all.jsonl" > "$dir/engine.log" 2>&1 &
    ENGINE_PID=$!
    for _ in $(seq 50); do
        "$BIN/nocturne-ctl" --port "$PORT" snapshot > /dev/null 2>&1 && return
        kill -0 "$ENGINE_PID" 2> /dev/null || break
        sleep 0.2
    done
    echo "engine did not start, see $dir/engine.log" >&2
    return 1
}

stop_engine() {
    [[ -n $ENGINE_PID ]] || return 0
    kill -TERM "$ENGINE_PID" 2> /dev/null || true
    wait "$ENGINE_PID" 2> /dev/null || true
    ENGINE_PID=
    command rm -rf "$WORK"
}

# The three mappings: screen 1 one keystoned slice, screen 2 two tilted halves, screen 3 the four quadrants as cube faces.
# One output shows all three mappings side by side: keystone, two tilted halves, four cube faces.
setup_one_output() {
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

setup_outputs() {
    [ ${#OUTPUTS[@]} -eq 1 ] && { setup_one_output; return; }
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
}

# fx LAYER INDEX: the INDEXth effect of the cycle.
fx() {
    # shellcheck disable=SC2086
    ctl fx "$1" ${EFFECTS[$(($2 % ${#EFFECTS[@]}))]}
}

sample_system() {
    local pid=$1
    set +e +o pipefail
    while kill -0 "$pid" 2> /dev/null; do
        local gpu ws app speed procs analysis spotlight
        gpu=$(ioreg -r -c IOAccelerator -d 1 | grep -oE '"Device Utilization %"=[0-9]+' | head -1 | cut -d= -f2)
        procs=$(ps -A -o %cpu=,comm=)
        ws=$(awk '/WindowServer$/ {print $1; exit}' <<< "$procs")
        # macOS analysing newly copied video, and Spotlight indexing it: both skew the numbers when busy.
        analysis=$(awk '/mediaanalysisd$/ {s += $1} END {print s + 0}' <<< "$procs")
        spotlight=$(awk '/\/(mds|mds_stores|mdworker|mdworker_shared)$/ {s += $1} END {print s + 0}' <<< "$procs")
        app=$(ps -o %cpu= -p "$pid" | tr -d ' ')
        speed=$(pmset -g therm | awk -F= '/CPU_Speed_Limit/ {gsub(/[ \t]/, "", $2); print $2}')
        printf '{"gpuUtil":%s,"windowServerCpu":%s,"engineCpu":%s,"cpuSpeedLimit":%s,"mediaAnalysisCpu":%s,"spotlightCpu":%s}\n' \
            "${gpu:-null}" "${ws:-null}" "${app:-null}" "${speed:-null}" "$analysis" "$spotlight"
        sleep 1
    done
}

# measure DIR SECONDS [ACTION]: keeps the engine's per-second stats and system samples of the next SECONDS, running
# ACTION (a function taking the seconds) meanwhile, or sleeping.
measure() {
    local dir=$1 seconds=$2 action=${3:-sleep}
    local first sampler
    first=$(($(wc -l < "$dir/all.jsonl") + 1))
    sample_system "$ENGINE_PID" > "$dir/sys.jsonl" &
    sampler=$!
    "$action" "$seconds"
    kill "$sampler" 2> /dev/null || true
    wait "$sampler" 2> /dev/null || true
    sed -n "${first},\$p" "$dir/all.jsonl" > "$dir/stats.jsonl"
}

# typical: the bottom layer plays the one long 1080p60 clip with Random Jump every 8 beats by up to 2; the others play
# 30 fps loops.
TYPICAL_GRID=("jump60_long jump60_long jump60_long jump60_long"
              "a30_short e30_short f30_long b30_long"
              "b30_long a30_short e30_short f30_long"
              "e30_short f30_long b30_long a30_short"
              "f30_long b30_long a30_short e30_short")
# heavy: Random Jump every beat by up to 8, and 60 fps loops in a quarter of the other cells.
HEAVY_GRID=("jump60_long jump60_long jump60_long jump60_long"
            "a30_short c60_short e30_short f30_long"
            "b30_long d60_long a30_short c60_short"
            "c60_short e30_short f30_long b30_long"
            "d60_long f30_long b30_long e30_short")

# setup_show JUMP ROW1..ROW5: 5 layers with 2-3 effects each, layers 4 and 5 in a group blended with screen, 4 columns
# of BPM-synced clips (one row per layer, bottom first), Random Jump on the bottom layer.
setup_show() {
    local jump=$1 layer column
    shift
    ctl bpm "$BPM"
    ctl quantise bar
    for _ in 2 3 4 5; do ctl addlayer; done
    ctl addgroup
    ctl movelayer 4 6 0
    ctl movelayer 5 6 1
    for layer in 1 2 3 4 5; do
        read -r -a row <<< "${!layer}"
        for column in 1 2 3 4; do
            ctl cell "$layer" "$column" "$(clip "${row[$((column - 1))]}")"
        done
    done
    # shellcheck disable=SC2086
    for column in 1 2 3 4; do ctl jump 1 $jump "$column"; done
    fx 1 0; fx 1 1
    fx 2 2; fx 2 1; fx 2 0
    fx 3 3; fx 3 0
    fx 4 4; fx 4 1
    fx 5 5; fx 5 0; fx 5 1
    ctl blend 2 screen
    ctl blend 3 add
    ctl blend 6 screen
    ctl opacity 5 0.8
    setup_outputs
    ctl column 1
}

switch_columns() {
    local seconds=$1 interval column=1
    interval=$(echo "$(bar_seconds) * $SWITCH_BARS" | bc)
    local end=$((SECONDS + seconds))
    while ((SECONDS + ${interval%.*} <= end)); do
        sleep "$interval"
        column=$((column % 4 + 1))
        ctl column "$column"
    done
    if ((end > SECONDS)); then sleep $((end - SECONDS)); fi
}

# run_show NAME JUMP ROW1..ROW5: a column trigger every SWITCH_BARS bars while measuring.
run_show() {
    local dir="$OUT/$1"
    echo "$1: warm-up ${WARMUP}s, measuring ${DURATION}s"
    start_engine "$dir"
    shift
    setup_show "$@"
    [[ -z $RECORD ]] || ctl record start "codec=$RECORD" "path=$WORK/recording.mov"
    sleep "$WARMUP"
    measure "$dir" "$DURATION" switch_columns
    [[ -z $RECORD ]] || ctl record stop
    stop_engine
}

run_typical() {
    run_show typical "$JUMP" "${TYPICAL_GRID[@]}"
}

run_heavy() {
    run_show heavy "$HEAVY_JUMP" "${HEAVY_GRID[@]}"
}

# LAYERS layers with EFFECTS effects each from the cycle, each playing a different clip, Random Jump on the bottom one.
setup_stress() {
    local layers=$1 effects=$2 layer k
    ctl bpm "$BPM"
    for ((layer = 2; layer <= layers; layer++)); do ctl addlayer; done
    for ((layer = 1; layer <= layers; layer++)); do
        ctl cell "$layer" 1 "$(clip "${CLIPS[$(((layer - 1) % ${#CLIPS[@]}))]}")"
        for ((k = 0; k < effects; k++)); do fx "$layer" $(((layer - 1) * effects + k)); done
        ((layer == 1)) || ctl blend "$layer" screen
    done
    ctl jump 1 $JUMP 1
    setup_outputs
    ctl quantise off
    ctl column 1
}

run_stress() {
    local level
    for level in $LEVELS; do
        local dir="$OUT/stress/$level"
        echo "stress $level: warm-up ${LEVEL_WARMUP}s, measuring ${LEVEL_SECONDS}s"
        start_engine "$dir"
        setup_stress "${level%x*}" "${level#*x}"
        sleep "$LEVEL_WARMUP"
        measure "$dir" "$LEVEL_SECONDS"
        stop_engine
    done
}

write_machine() {
    printf '{"model":"%s","cpu":"%s","gpu":"%s","macos":"%s","displays":"%s","engineArgs":"%s","seed":%s,"record":"%s"}\n' \
        "$(sysctl -n hw.model)" "$(sysctl -n machdep.cpu.brand_string)" \
        "$(system_profiler SPDisplaysDataType | awk -F': ' '/Chipset Model/ {print $2; exit}')" \
        "$(sw_vers -productVersion)" "${OUTPUTS[*]}" "$ENGINE_ARGS" "$SEED" "$RECORD" > "$OUT/machine.json"
}

trap stop_engine EXIT
mkdir -p "$OUT"
write_machine
for scenario in $SCENARIOS; do
    "run_$scenario"
done
