#!/bin/bash
# The chaos rehearsal on the Mac mini, the parts that run without hands (bench/chaos.sh): client connections killed
# and stalled, the engine killed and hung, the state disk full, the media drive detached and back, all while the soak
# show plays at the F1 setup. About 8 minutes. Writes bench/results/chaos-<date>-<commit>.json and prints a table.
#
# The rest is manual, with a person watching: unplug and replug the display, power-cycle the
# projector, unplug the audio interface, pull the network cable.
#
# usage: tools/chaos-mini.sh [--locked]   takes the Mini lock, unless --locked (the caller holds it)
#   MINI=user@host or a host name (default: tools/mini-host.sh); DISPLAYS, ENGINE_ARGS and SETTLE pass through.
set -euo pipefail

. "$(dirname "$0")/mini-host.sh"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
REMOTE=Nocturne/chaos
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=10 "$MINI")

lock() {
    NOCTURNE_MINI=$MINI "$ROOT/tools/mini-lock.sh" "$@"
}

media=$("$ROOT/bench/make_media.sh")
held=${1:-}
[[ $held == --locked ]] || lock acquire "chaos-27"
cleanup() {
    "${SSH[@]}" "pkill -f '[n]octurne.*$REMOTE/' ; command rm -rf ~/$REMOTE" || true
    [[ $held == --locked ]] || lock release
}
trap cleanup EXIT
MINI=$MINI "$ROOT/tools/deploy-mini.sh" --locked
"${SSH[@]}" "command rm -rf ~/$REMOTE && mkdir -p ~/$REMOTE/media"
scp -q "$ROOT/bench/soak.sh" "$ROOT/bench/chaos.sh" "$MINI:$REMOTE/"
scp -q "$media"/*.mp4 "$MINI:$REMOTE/media/"
env=""
for name in DISPLAYS ENGINE_ARGS SETTLE; do
    if [[ -n ${!name+x} ]]; then env+="$name=$(printf %q "${!name}") "; fi
done
"${SSH[@]}" "$env ~/$REMOTE/chaos.sh ~/Nocturne/Nocturne.app/Contents/MacOS ~/$REMOTE/media ~/$REMOTE/raw"
raw=$(mktemp -d)
scp -q -r "$MINI:$REMOTE/raw/." "$raw/"
mkdir -p "$ROOT/bench/results"
result="$ROOT/bench/results/chaos-$(date +%Y%m%d-%H%M)-$(git -C "$ROOT" rev-parse --short HEAD).json"
"$ROOT/bench/chaos_report.py" "$raw" > "$result"
command rm -rf "$raw"
echo "result: ${result#"$ROOT"/}"
