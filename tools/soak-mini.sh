#!/bin/bash
# The soak test on the Mac mini (bench/soak.sh): Autopilot, switching, transitions, modulators and Random Jump at the
# F1 setup under the supervisor, MINUTES long (default 30), sampled every second and every minute.
#
# usage: tools/soak-mini.sh start [--locked]
#                                      waits for the Mini lock and keeps it for the whole soak (--locked: the caller
#                                      holds it already, with an owner starting "soak"); deploys the build
#                                      (tools/deploy-mini.sh), copies the benchmark clips, waits until mediaanalysisd
#                                      has been below 10% CPU for 3 minutes and starts the soak detached on the Mini
#                                      (bench/soak.sh releases the lock when it ends)
#        tools/soak-mini.sh status     progress: the soak's log and its last minute sample
#        tools/soak-mini.sh fetch      once done: copies the raw data to bench/soak/<id>/ (not committed), writes
#                                      bench/results/soak-<id>.json and prints the verdict; deletes the Mini's copy
#   MINI=user@host or a host name (default: tools/mini-host.sh); MINUTES, DISPLAYS, ENGINE_ARGS, SEED, BPM, SWITCH_BARS,
#   PREVIEW_CLIP, PREVIEWS pass
#   through (default three virtual windows; real displays: DISPLAYS="id1 id2 id3" ENGINE_ARGS="").
set -euo pipefail

. "$(dirname "$0")/mini-host.sh"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
REMOTE=Nocturne/soak
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=10 "$MINI")
PASSED_ENV=(MINUTES DISPLAYS ENGINE_ARGS SEED BPM SWITCH_BARS PREVIEW_CLIP PREVIEWS)

lock() {
    NOCTURNE_MINI=$MINI "$ROOT/tools/mini-lock.sh" "$@"
}

remote_env() {
    local name
    for name in "${PASSED_ENV[@]}"; do
        if [[ -n ${!name+x} ]]; then printf '%s=%q ' "$name" "${!name}"; fi
    done
}

acquire_lock() {
    until lock acquire "soak-27 $(git -C "$ROOT" rev-parse --short HEAD)" 2> /dev/null; do
        echo "waiting for the Mini: $(lock status | tr '\n' ' ')"
        sleep 60
    done
}

# mediaanalysisd analyses newly copied video for a while and skews every measurement. It is waited for at most
# QUIET_WAIT minutes (default 30); the minute samples record it either way.
wait_for_quiet() {
    local quiet=0 cpu waited=0
    while ((quiet < 3)); do
        if ((waited++ >= ${QUIET_WAIT:-30})); then
            echo "mediaanalysisd still busy after ${QUIET_WAIT:-30} minutes; starting anyway"
            return
        fi
        cpu=$("${SSH[@]}" "ps -A -o %cpu=,comm= | awk '/mediaanalysisd\$/ {s += \$1} END {print s + 0}'")
        if (($(echo "$cpu < 10" | bc))); then quiet=$((quiet + 1)); else quiet=0; fi
        echo "mediaanalysisd ${cpu}% CPU ($quiet quiet minutes of 3)"
        if ((quiet < 3)); then sleep 60; fi
    done
}

start() {
    local media id
    media=$("$ROOT/bench/make_media.sh")
    id=$(date +%Y%m%d-%H%M)-$(git -C "$ROOT" rev-parse --short HEAD)
    [[ ${1:-} == --locked ]] || acquire_lock
    MINI=$MINI "$ROOT/tools/deploy-mini.sh" --locked
    "${SSH[@]}" "command rm -rf ~/$REMOTE && mkdir -p ~/$REMOTE/media && touch ~/$REMOTE/.metadata_never_index"
    scp -q "$ROOT/bench/soak.sh" "$MINI:$REMOTE/"
    scp -q "$media"/*.mp4 "$MINI:$REMOTE/media/"
    wait_for_quiet
    "${SSH[@]}" "echo $id > ~/$REMOTE/id; $(remote_env) nohup ~/$REMOTE/soak.sh ~/Nocturne/Nocturne.app/Contents/MacOS \
        ~/$REMOTE/media ~/$REMOTE/raw > ~/$REMOTE/soak.log 2>&1 < /dev/null &"
    echo "soak $id started on $MINI; tools/soak-mini.sh status shows progress"
}

status() {
    "${SSH[@]}" "cat ~/$REMOTE/id 2> /dev/null; tail -3 ~/$REMOTE/soak.log; echo \"\$(wc -l < ~/$REMOTE/raw/minutes.jsonl) minutes sampled\";
        tail -1 ~/$REMOTE/raw/minutes.jsonl; tail -1 ~/$REMOTE/raw/stats.jsonl | cut -c1-400; ls ~/$REMOTE/raw/done 2> /dev/null && echo done"
}

fetch() {
    local id raw result status=0
    id=$("${SSH[@]}" "cat ~/$REMOTE/id")
    "${SSH[@]}" "test -f ~/$REMOTE/raw/done" || { echo "soak $id is still running" >&2; exit 2; }
    raw="$ROOT/bench/soak/$id"
    mkdir -p "$raw"
    scp -q -r "$MINI:$REMOTE/raw/." "$raw/"
    scp -q "$MINI:$REMOTE/soak.log" "$raw/"
    result="$ROOT/bench/results/soak-$id.json"
    "$ROOT/bench/soak_report.py" "$raw" "${id##*-}" > "$result" || status=1
    "${SSH[@]}" "command rm -rf ~/$REMOTE"
    echo "raw data: ${raw#"$ROOT"/}; summary: ${result#"$ROOT"/}"
    return "$status"
}

case ${1:-} in
    start) start "${2:-}" ;;
    status) status ;;
    fetch) fetch ;;
    *) sed -n '2,17p' "$0"; exit 2 ;;
esac
