#!/bin/bash
# Plays the odd-media corpus (tests/make_odd_media.sh) on the Mac mini with the build deployed there
# (tools/deploy-mini.sh): copies it to ~/Nocturne/odd-media, runs the corpus test, prints one line per file and deletes
# the copy.
#
# usage: tools/odd-media-mini.sh [--locked]    takes the Mini lock, unless --locked (the caller holds it)
#   MINI=user@host or a host name (default: tools/mini-host.sh)
set -euo pipefail

. "$(dirname "$0")/mini-host.sh"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=10 "$MINI")

lock() {
    NOCTURNE_MINI=$MINI "$ROOT/tools/mini-lock.sh" "$@"
}

corpus=$("$ROOT/tests/make_odd_media.sh")
held=${1:-}
[[ $held == --locked ]] || lock acquire odd-media-mini
cleanup() {
    "${SSH[@]}" 'command rm -rf ~/Nocturne/odd-media' || true
    [[ $held == --locked ]] || lock release
}
trap cleanup EXIT
"${SSH[@]}" 'command rm -rf ~/Nocturne/odd-media && mkdir -p ~/Nocturne/odd-media'
scp -q "$corpus"/* "$MINI:Nocturne/odd-media/"
"${SSH[@]}" 'cd ~/Nocturne/tests && NOCTURNE_ODD_MEDIA_DIR=~/Nocturne/odd-media NOCTURNE_MEDIA_DIR=$PWD/media \
    NOCTURNE_GOLDEN_DIR=$PWD/golden ./nocturne_tests -tc="*odd-media corpus*"' 2>&1 |
    grep -E 'MESSAGE|ERROR|values|test cases' | sed 's/.*MESSAGE: //'
