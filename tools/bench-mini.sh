#!/bin/bash
# Builds Universal, deploys it to the Mac mini (tools/deploy-mini.sh, ~/Nocturne/Nocturne.app) with the benchmark
# media in ~/Nocturne/bench, runs the benchmark there, stores the result in bench/results/<date>-<commit>.json, compares
# it with the last good/* tag that has a result (else bench/baseline.json) and prints the verdict. The Mini's
# ~/Nocturne/bench is deleted afterwards; the app stays. Exits 1 on FAIL.
#
# usage: tools/bench-mini.sh [typical] [stress]     (default both)
#        tools/bench-mini.sh --verdict RESULT        only compare an existing result
#   MINI=user@host or a host name (default: tools/mini-host.sh); run.sh's settings (DISPLAYS, ENGINE_ARGS, SEED,
#   DURATION, WARMUP, SWITCH_BARS, LEVELS, LEVEL_SECONDS, ...) pass through to the Mini.
set -euo pipefail

. "$(dirname "$0")/mini-host.sh"
REMOTE=Nocturne/bench
REMOTE_BIN=Nocturne/Nocturne.app/Contents/MacOS
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=10 "$MINI")
PASSED_ENV=(DISPLAYS ENGINE_ARGS SEED PORT BPM WARMUP DURATION SWITCH_BARS JUMP LEVELS LEVEL_WARMUP LEVEL_SECONDS
            RECORD)

check_route() {
    local host=${MINI#*@}
    [[ $host =~ ^(192\.168|10|172\.(1[6-9]|2[0-9]|3[01])|169\.254)\. ]] || return 0
    if ! route -n get "$host" 2> /dev/null | grep -q 'interface: en0'; then
        echo "bench-mini: $host is not routed over en0 (Cloudflare WARP captures LAN routes?); fix the route first" >&2
        exit 2
    fi
}

build() {
    nice -n 15 cmake -S "$ROOT" -B "$ROOT/build" > /dev/null
    nice -n 15 cmake --build "$ROOT/build" -j8 > /dev/null
}

remote_env() {
    local name
    for name in "${PASSED_ENV[@]}"; do
        if [[ -n ${!name+x} ]]; then printf '%s=%q ' "$name" "${!name}"; fi
    done
}

lock() {
    NOCTURNE_MINI=$MINI "$ROOT/tools/mini-lock.sh" "$@"
}

# Waits up to 30 minutes for the Mini's exclusive lock.
acquire_lock() {
    local tries=0
    until lock acquire bench-mini 2> /dev/null; do
        ((++tries <= 60)) || { echo "bench-mini: the Mini stayed busy" >&2; exit 2; }
        echo "waiting for the Mini: $(lock status | tr '\n' ' ')"
        sleep 30
    done
}

warn_if_loaded() {
    local load
    load=$(sysctl -n vm.loadavg | awk '{print $2}')
    if (($(echo "$load > 2" | bc))); then
        echo "bench-mini: warning: laptop load average $load > 2; treat the numbers as provisional"
    fi
}

cleanup_remote() {
    "${SSH[@]}" "pkill -f '[n]octurne .*$REMOTE/' ; rm -rf ~/$REMOTE" || true
    lock release || true
}

deploy() {
    local media=$1
    MINI=$MINI "$ROOT/tools/deploy-mini.sh" --locked
    "${SSH[@]}" "pkill -f '[n]octurne .*$REMOTE/' ; rm -rf ~/$REMOTE && mkdir -p ~/$REMOTE/media"
    scp -q "$ROOT/bench/run.sh" "$MINI:$REMOTE/"
    scp -q "$media"/*.mp4 "$MINI:$REMOTE/media/"
}

# The newest result in the newest good/* tag that has one, else bench/baseline.json, written to $1; fails if none.
baseline() {
    local tag file
    for tag in $(git -C "$ROOT" tag -l 'good/*' --sort=-creatordate); do
        file=$(git -C "$ROOT" ls-tree --name-only "$tag" bench/results/ | { grep -v -- -record- || true; } | sort | tail -1)
        if [[ -n $file ]]; then
            git -C "$ROOT" show "$tag:$file" > "$1"
            echo "baseline: $file in $tag"
            return
        fi
    done
    [[ -f $ROOT/bench/baseline.json ]] || return 1
    cp "$ROOT/bench/baseline.json" "$1"
    echo "baseline: bench/baseline.json"
}

main() {
    local scenarios=${*:-typical stress}
    local commit dirty=0 media raw result
    check_route
    commit=$(git -C "$ROOT" rev-parse --short HEAD)
    [[ -z $(git -C "$ROOT" status --porcelain) ]] || dirty=1
    echo "== build"
    build
    echo "== media"
    media=$("$ROOT/bench/make_media.sh")
    warn_if_loaded
    echo "== deploy to $MINI"
    acquire_lock
    trap cleanup_remote EXIT
    deploy "$media"
    echo "== run: $scenarios"
    "${SSH[@]}" "$(remote_env) ~/$REMOTE/run.sh ~/$REMOTE_BIN ~/$REMOTE/media ~/$REMOTE/raw $scenarios"
    raw=$(mktemp -d)
    scp -q -r "$MINI:$REMOTE/raw/." "$raw/"
    cleanup_remote
    trap - EXIT
    mkdir -p "$ROOT/bench/results"
    result="$ROOT/bench/results/$(date +%Y%m%d-%H%M)-$commit${RECORD:+-record-$RECORD}.json"
    "$ROOT/bench/report.py" summarize "$raw" "$commit" "$dirty" > "$result"
    command rm -rf "$raw"
    echo "== result: ${result#"$ROOT"/}"
    verdict "$result"
}

verdict() {
    local base status=0
    base=$(mktemp)
    if baseline "$base"; then
        "$ROOT/bench/report.py" compare "$1" "$base" || status=1
    else
        "$ROOT/bench/report.py" compare "$1" || status=1
    fi
    command rm -f "$base"
    return "$status"
}

if [[ ${1:-} == --verdict ]]; then
    verdict "$2"
else
    main "$@"
fi
