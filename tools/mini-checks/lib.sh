#!/bin/bash
# Sourced by every tools/mini-checks script. Runs on the Mac mini (bash 3.2, jq from macOS 15), against the engine the
# autostart agent runs there. Every change a check makes is reverted; nothing is deleted.
#
# Options (every script): --dry-run   print what would run, read nothing from the engine and change nothing
#                         --no-restarts  skip the checks that restart the engine (flag A/Bs, input persistence)
# Environment: AB_SECONDS (60) measured seconds per A/B side, WARMUP (10), SAMPLES (dry-run: read ctl replies and
# stats from this folder instead, to test the parsers), OUT (results folder), CTL, STATS, PLIST.

APP=${NOCTURNE_APP:-$HOME/Nocturne/Nocturne.app}
CTL=${CTL:-$APP/Contents/MacOS/nocturne-ctl}
LOGS=${LOGS:-$HOME/Library/Logs/Nocturne}
STATS=${STATS:-$LOGS/stats.jsonl}
STATE=${STATE:-$HOME/Library/Application Support/Nocturne/State}
PLIST=${PLIST:-$HOME/Library/LaunchAgents/ai.azylume.nocturne.plist}
LABEL=ai.azylume.nocturne
DOMAIN=gui/$(id -u)
AB_SECONDS=${AB_SECONDS:-60}
WARMUP=${WARMUP:-10}
DRY=${DRY:-0}
RESTARTS=${RESTARTS:-1}
SAMPLES=${SAMPLES:-}

parse_args() {
    local a
    for a in "$@"; do
        case $a in
            --dry-run) DRY=1 ;;
            --no-restarts) RESTARTS=0 ;;
            -h | --help) sed -n '2,/^[^#]/p' "$0" | sed -e '$d' -e 's/^# \{0,1\}//'; sed -n '4,8p' "$HERE/lib.sh" | sed 's/^# \{0,1\}//'; exit 0 ;;
            *) echo "unknown option $a" >&2; exit 2 ;;
        esac
    done
    export DRY RESTARTS
    if [[ -z ${OUT:-} ]]; then
        if ((DRY)); then
            OUT=$(mktemp -d "${TMPDIR:-/tmp}/mini-checks-dry.XXXXXX")
        else
            OUT=$HOME/Nocturne/mini-checks-results/$(date +%Y%m%d-%H%M%S)
        fi
    fi
    mkdir -p "$OUT"
    RESULTS=${RESULTS:-$OUT/results.tsv}
    export OUT RESULTS
    ((DRY)) || command -v jq > /dev/null || { echo "mini-checks: needs jq (macOS 15 has /usr/bin/jq)" >&2; exit 1; }
    ((DRY)) || [[ -x $CTL ]] || { echo "mini-checks: no $CTL; deploy first" >&2; exit 1; }
}

say() { printf '%s\n' "$*" >&2; }

# One table row: NAME, COMMAND, VALUE, THRESHOLD, RESULT (PASS, FAIL, SKIP, MANUAL, INFO).
row() {
    printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$5" >> "$RESULTS"
    say "  $5  $1: $3 ($4)"
}

manual() { row "$1" "$2" "-" "$3" MANUAL; }

# A change to the show or the machine: printed under --dry-run, logged otherwise.
act() {
    if ((DRY)); then
        say "[dry-run] $*"
        return 0
    fi
    printf '%s %s\n' "$(date +%T)" "$*" >> "$OUT/actions.log"
    "$@"
}

pause() {
    if ((DRY)); then
        say "[dry-run] wait $1 s"
    else
        sleep "$1"
    fi
}

now() { date +%s; }
now_ms() { perl -MTime::HiRes=time -e 'printf "%d\n", time * 1000'; }

# ctl's output as JSON lines (the snapshot first for most commands). Under --dry-run it reads
# $SAMPLES/ctl-WORD1[-WORD2].jsonl when there is one, else prints nothing.
ctl() {
    if ((DRY)); then
        say "[dry-run] nocturne-ctl $*"
        local f=$SAMPLES/ctl-$1${2:+-$2}.jsonl
        [[ -n $SAMPLES && -f $f ]] && cat "$f"
        return 0
    fi
    printf '%s nocturne-ctl %s\n' "$(date +%T)" "$*" >> "$OUT/actions.log"
    "$CTL" "$@" 2> "$OUT/ctl.err"
}

# The last message of TYPE that ctl printed for the command.
ctl_reply() {
    local type=$1
    shift
    ctl "$@" | jq -c --arg t "$type" 'select(.type == $t)' 2> /dev/null | tail -n 1
}

snapshot() { ctl_reply snapshot snapshot; }

# Stats lines between two unix times (both files of the rotating log); every line under --dry-run with SAMPLES.
stats_between() {
    if ((DRY)); then
        [[ -n $SAMPLES && -f $SAMPLES/stats.jsonl ]] && cat "$SAMPLES/stats.jsonl"
        return 0
    fi
    cat "$STATS.1" "$STATS" 2> /dev/null |
        jq -cR --argjson a "$1" --argjson b "$2" 'fromjson? | select(.unix >= $a and .unix < $b)'
}

# Waits SECONDS and keeps that window's stats lines in $OUT/stats-NAME.jsonl; prints the file.
measure() {
    local from
    from=$(now)
    pause "$2"
    measure_since "$1" "$from"
}

# Keeps the stats lines from unix time FROM until now in $OUT/stats-NAME.jsonl; prints the file.
measure_since() {
    stats_between "$2" "$(now)" > "$OUT/stats-$1.jsonl"
    printf '%s\n' "$OUT/stats-$1.jsonl"
}

# One JSON object summarising a stats file: render, CPU, previews, and every attached output.
summarize() {
    jq -s '
      def avg: if length > 0 then add / length else null end;
      def r2: if . == null then null else (. * 100 | round / 100) end;
      {seconds: length,
       fps: (map(.render.fps) | avg | r2),
       gpuMs: (map(.render.gpuMs) | avg | r2),
       gpuSteadyMs: (map(select((.sizes.fading // 0) == 0) | .render.gpuMs) | avg | r2),
       gpuFadeMs: (map(select((.sizes.fading // 0) > 0) | .render.gpuMs) | avg | r2),
       budgetMax: (map(.render.budgetLevel) | max),
       passesMax: (map(.render.passes) | max),
       cpu: (map(.cpu) | avg | r2),
       previewsMs: (map(.render.previews.gpuMs // 0) | avg | r2),
       watchersMax: (map([.render.previews.watchers // {} | .[]] | add // 0) | max),
       recordDropped: (map(.record.dropped // 0) | add),
       outputs: ([.[] | .outputs[]? | select(.attached)] | group_by(.display) | map({
         display: .[0].display,
         presents: (map(.presents) | avg | r2),
         new: (map(.new) | avg | r2),
         newMin: (map(.new) | min | r2),
         hitches: (map(.hitches) | add),
         hitchesPerS: ((map(.hitches) | add) / length | r2),
         gpuMs: (map(.gpuMs) | avg | r2)}))}' "$1"
}

# Waits for SECONDS steady seconds (no fade, no compile; at most 4x as long) and summarises those seconds:
# steady_window NAME SECONDS
steady_window() {
    local name=$1 n=$2 from f
    from=$(now)
    pause "$n"
    while :; do
        f=$(measure_since "$name" "$from")
        ((DRY || $(now) - from >= 4 * n)) && break
        (($(jq -s 'map(select((.sizes.fading // 0) == 0 and (.render.compiles // 0) == 0)) | length' "$f") >= n)) && break
        pause 2
    done
    jq -c 'select((.sizes.fading // 0) == 0 and (.render.compiles // 0) == 0)' "$f" | head -n "$n" \
        > "$f.steady"
    summarize "$f.steady"
}

# The median of signed numbers with two decimals, empty without any: median N...
median() {
    (($#)) || return 0
    printf '%s\n' "$@" | sort -g | awk '{ v[NR] = $1 } END { printf "%+.2f", NR % 2 ? v[(NR + 1) / 2] : (v[NR / 2] + v[NR / 2 + 1]) / 2 }'
}

# A JSON value, or {} when empty.
obj() { if [[ -n $1 ]]; then printf '%s\n' "$1"; else echo '{}'; fi; }

# One field of a JSON value, "-" when missing: field FILTER JSON
field() { jq -r "($1) | if . == null then \"-\" else . end" <<< "$(obj "$2")" 2> /dev/null || echo -; }

# Compares two numbers: num A OP B (OP <=, >=, <, >)
num() { awk -v a="$1" -v b="$3" "BEGIN { exit !((a + 0) $2 (b + 0)) }"; }

pass_if() { if [[ $1 != - && $1 != null && -n $1 ]] && num "$1" "$2" "$3"; then echo PASS; else echo FAIL; fi; }

engine_running() {
    ((DRY)) && return 0
    launchctl print "$DOMAIN/$LABEL" > /dev/null 2>&1
}

# --- Engine restarts through the autostart plist (the only place engine flags come from) ---

PLIST_ORIG=
plist_changed=0

plist_backup() {
    PLIST_ORIG=$OUT/ai.azylume.nocturne.plist.orig
    [[ -f $PLIST_ORIG ]] && return 0
    if ((DRY)); then
        say "[dry-run] cp $PLIST $PLIST_ORIG"
        return 0
    fi
    cp -p "$PLIST" "$PLIST_ORIG"
}

# The plist's ProgramArguments, one per line (from PLIST_ORIG, or the samples under --dry-run).
plist_args() {
    local src=${1:-$PLIST_ORIG}
    ((DRY)) && [[ -n $SAMPLES && -f $SAMPLES/autostart.plist ]] && src=$SAMPLES/autostart.plist
    [[ -f $src ]] || return 0
    plutil -extract ProgramArguments json -o - "$src" | jq -r '.[]'
}

# Writes $OUT/NAME.plist: the original arguments without FLAG, plus FLAG when WITH is 1.
plist_variant() {
    local flag=$1 with=$2 name=$3 work args
    work=$OUT/$name.plist
    local src=$PLIST_ORIG
    ((DRY)) && [[ -n $SAMPLES && -f $SAMPLES/autostart.plist ]] && src=$SAMPLES/autostart.plist
    cp "$src" "$work" 2> /dev/null || { say "no $src"; return 1; }
    args=$(plutil -extract ProgramArguments json -o - "$src" |
        jq -c --arg f "$flag" --argjson w "$with" 'map(select(. != $f)) + (if $w == 1 then [$f] else [] end)')
    plutil -replace ProgramArguments -json "$args" "$work" && plutil -lint "$work" > /dev/null || return 1
    printf '%s\n' "$work"
}

# Loads a plist as the autostart agent (bootout, copy, bootstrap, as tools/autostart.sh does) and waits for the
# engine's first rendering second after it.
agent_load() {
    local plist=$1 since i
    act launchctl bootout "$DOMAIN/$LABEL" 2> /dev/null || true
    since=$(($(now) + 1))
    act cp "$plist" "$PLIST"
    for i in 1 2 3 4 5; do
        act launchctl bootstrap "$DOMAIN" "$PLIST" 2> /dev/null && break
        ((i == 5)) && { say "cannot bootstrap $PLIST"; return 1; }
        sleep 2
    done
    wait_rendering "$since"
}

# Waits up to 90 s for a stats second after SINCE that renders at 50 fps or more.
wait_rendering() {
    ((DRY)) && { say "[dry-run] wait for the engine to render"; return 0; }
    local since=$1 i
    for i in $(seq 1 90); do
        if stats_between "$since" 99999999999 | jq -e 'select(.render.fps >= 50)' > /dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    say "the engine did not render within 90 s"
    return 1
}

plist_restore() {
    ((plist_changed)) || return 0
    say "restoring the original autostart plist"
    agent_load "$PLIST_ORIG" || say "RESTORE FAILED: run  cp '$PLIST_ORIG' '$PLIST' && ~/Nocturne/tools/autostart.sh restart"
    if ((!DRY)) && ! same_file "$PLIST_ORIG" "$PLIST" 2> /dev/null; then
        say "RESTORE FAILED: $PLIST differs from $PLIST_ORIG"
    fi
    plist_changed=0
}

same_file() { /usr/bin/cmp -s "$1" "$2"; }

run_with_plist() {
    plist_changed=1
    agent_load "$1"
}
