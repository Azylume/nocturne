#!/bin/bash
# On-screen demo of a synced show on the Mac mini (tools/sync-show.sh first): starts the engine on
# ~/Nocturne/shows/NAME with --virtual N windows (or DISPLAY_ID's real output) and --debug-frame-bits, triggers the
# first PER_DECK columns of every deck in turn, one every STEP seconds, through the beat clock's quantisation, records
# the engine's per-second stats and system samples, stops the engine and prints the summary (bench/report.py's
# measures) and the hitches of the seconds a trigger landed in against the others. With AHEAD=1 the next deck is
# selected a step before its first column is triggered, as a VJ would, so the engine preloads it; ARM=1 also arms it.
# Virtual windows are composited by WindowServer, which costs GPU time that real outputs would not.
#
# usage: tools/demo-show.sh SHOW.json [SECONDS]   SHOW.json: the local file synced; SECONDS measured (default 20)
#   MINI=user@host, VIRTUAL [5], DISPLAY_ID [none: virtual windows], STEP [8], PER_DECK [6], WARMUP [10],
#   QUANTISE [bar; off|beat|bar|NBARS|BEATS], TRIGGERS [1; 0 plays the first column only], AHEAD [0], ARM [0],
#   OUT [a new temporary folder], LFOS [1]: replace audio modulators on layer opacity by beat LFOs, for a Mini with no
#   audio input, LOCKED [0]: 1 when the caller holds the Mini lock, TRANSITION [none; e.g. "alpha 4"]: every layer's
#   transition, ENGINE_ARGS [none; e.g. --gpu-profile]: more engine options
set -euo pipefail

SHOW=${1:?usage: tools/demo-show.sh SHOW.json [SECONDS]}
DURATION=${2:-20}
. "$(dirname "$0")/mini-host.sh"
VIRTUAL=${VIRTUAL:-5}
STEP=${STEP:-8}
PER_DECK=${PER_DECK:-6}
WARMUP=${WARMUP:-10}
ARM=${ARM:-0}
AHEAD=${AHEAD:-0}
QUANTISE=${QUANTISE:-bar}
TRIGGERS=${TRIGGERS:-1}
DISPLAY_ID=${DISPLAY_ID:-}
LFOS=${LFOS:-1}
LOCKED=${LOCKED:-0}
TRANSITION=${TRANSITION:-}
ENGINE_ARGS=${ENGINE_ARGS:-}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$(mktemp -d "${TMPDIR:-/tmp}/nocturne-demo.XXXXXX")}
NAME=$(basename "$SHOW")
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=10 "$MINI")

# Prints the demo's ctl commands: "setup ..." once, then "step ..." lines that run in a loop until time is up.
read -r -d '' PLAN_PY << 'EOF' || true
import json, sys
c = json.load(open(sys.argv[1]))["composition"]
per, arm, lfos, ahead, triggers = (int(sys.argv[2]), *(a == "1" for a in sys.argv[3:7]))
print(f"setup ctl quantise {sys.argv[7]}")
for layer in c["layers"] if sys.argv[8] else []:
    print(f"setup ctl transition {layer['id']} {sys.argv[8]}")
if lfos:
    shapes = ["sine 2", "sine 4", "saw 1", "square 2"]
    audio = [m for m in c["modulators"] if m["source"] == "audio" and m["target"].get("param") == "opacity"]
    for i, m in enumerate(audio):
        print(f"setup ctl mod rm {m['id']}; ctl mod lfo {m['target']['layer']}.opacity {shapes[i % len(shapes)]}")
steps = [(d["id"], col["id"]) for d in c["decks"] for col in d["columns"][:per]]
steps = steps[1:] + steps[:1]
def cells(column):
    return [layer["id"] for layer in c["layers"] if str(column) in layer["clips"]]
if not triggers:
    print("step true")
    steps = []
for i, (deck, column) in enumerate(steps):
    cmds = [f"ctl deck {deck}", f"ctl column {column}"]
    nxt_deck, nxt = steps[(i + 1) % len(steps)]
    prev_deck, _ = steps[i - 1]
    if arm and prev_deck != deck:
        cmds += [f"ctl arm {layer} {column} off" for layer in cells(column)]
    if arm and nxt_deck != deck:
        cmds += [f"ctl arm {layer} {nxt}" for layer in cells(nxt)]
    if ahead and nxt_deck != deck:
        cmds.append(f"ctl deck {nxt_deck}")
    print("step " + "; ".join(cmds) + f"; mark {deck} {column}")
EOF
plan=$(python3 -c "$PLAN_PY" "$SHOW" "$PER_DECK" "$ARM" "$LFOS" "$AHEAD" "$TRIGGERS" "$QUANTISE" "$TRANSITION")
OUTPUTS="--virtual $VIRTUAL"
[[ -z $DISPLAY_ID ]] || OUTPUTS="--display $DISPLAY_ID"

remote=$(mktemp)
cleanup() {
    command rm -f "$remote"
    [[ $LOCKED == 1 ]] || NOCTURNE_MINI=$MINI "$ROOT/tools/mini-lock.sh" release
}
[[ $LOCKED == 1 ]] || NOCTURNE_MINI=$MINI "$ROOT/tools/mini-lock.sh" acquire demo-show
trap cleanup EXIT

{
    cat << EOF
set -euo pipefail
BIN=~/Nocturne/Nocturne.app/Contents/MacOS
D=~/Nocturne/demo
command rm -rf "\$D" && mkdir -p "\$D/stats"
ctl() { "\$BIN/nocturne-ctl" --port 7400 "\$@" > /dev/null; }
mark() { echo "\$((\$(wc -l < "\$D/all.jsonl") - first + 1)) deck \$1 column \$2" >> "\$D/stats/triggers.txt"; }
sample() {
    set +e +o pipefail
    while kill -0 \$ENGINE 2> /dev/null; do
        gpu=\$(ioreg -r -c IOAccelerator -d 1 | grep -oE '"Device Utilization %"=[0-9]+' | head -1 | cut -d= -f2)
        ws=\$(ps -A -o %cpu=,comm= | awk '/WindowServer\$/ {print \$1; exit}')
        app=\$(ps -o %cpu= -p \$ENGINE | tr -d ' ')
        speed=\$(pmset -g therm | awk -F= '/CPU_Speed_Limit/ {gsub(/[ \t]/, "", \$2); print \$2}')
        printf '{"gpuUtil":%s,"windowServerCpu":%s,"engineCpu":%s,"cpuSpeedLimit":%s}\n' \
            "\${gpu:-null}" "\${ws:-null}" "\${app:-null}" "\${speed:-null}"
        sleep 1
    done
}
"\$BIN/nocturne" $OUTPUTS --debug-frame-bits --composition ~/Nocturne/shows/$NAME --media-root ~/Nocturne/media \\
    --library "\$D/library" --presets "\$D/presets.json" --stats-json "\$D/all.jsonl" \\
    $ENGINE_ARGS > "\$D/stats/engine.log" 2>&1 &
ENGINE=\$!
for _ in \$(seq 100); do ctl snapshot 2> /dev/null && break; sleep 0.2; done
EOF
    sed -n 's/^setup //p' <<< "$plan"
    cat << EOF
sleep $WARMUP
first=\$((\$(wc -l < "\$D/all.jsonl") + 1))
sample > "\$D/stats/sys.jsonl" &
SAMPLER=\$!
end=\$((SECONDS + $DURATION))
while (( SECONDS < end )); do
EOF
    sed -n "s/^step \(.*\)/    (( SECONDS < end )) || break; \1; sleep $STEP/p" <<< "$plan"
    cat << EOF
done
kill \$SAMPLER 2> /dev/null || true
sed -n "\${first},\\\$p" "\$D/all.jsonl" > "\$D/stats/stats.jsonl"
kill -TERM \$ENGINE; wait \$ENGINE || true
EOF
} > "$remote"

"${SSH[@]}" 'bash -s' < "$remote"
mkdir -p "$OUT"
scp -q -o BatchMode=yes "$MINI:Nocturne/demo/stats/*" "$OUT/"
"${SSH[@]}" 'command rm -rf ~/Nocturne/demo'
grep -vE '^ *(render|decode|output|audio) ' "$OUT/engine.log" | grep -iE "error|warn|cannot|missing|corrupt" | head -20 || true
python3 -c "import json, sys; sys.path.insert(0, '$ROOT/bench'); import report; from pathlib import Path; \
print(json.dumps(report.summarize_run(Path('$OUT')), indent=1))" | tee "$OUT/summary.json"
python3 "$ROOT/bench/trigger_seconds.py" "$OUT" | tee "$OUT/triggers.json"
echo "demo stats in $OUT"
