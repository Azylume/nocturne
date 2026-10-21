#!/bin/bash
# Installs the build on the Mac mini at its one fixed path, used by tests, demos, benchmarks and autostart alike:
#   ~/Nocturne/Nocturne.app     the engine, supervisor and ctl
#   ~/Nocturne/tools            tools/*.sh
#   ~/Nocturne/mini-checks      tools/mini-checks/, so the Mini never runs stale checks
#   ~/Nocturne/tests            nocturne_tests, goldens, test clips, the demo scripts and the source files tests read
# tools, mini-checks and tests are replaced; the app goes to ~/Nocturne/incoming and tools/update-bundle.sh installs it there, the
# same steps as the UI's Update: the running bundle is kept aside (a signed binary overwritten in place is killed at
# launch), a running autostart agent restarts on the new build with the live state, and a build that fails its health
# check is rolled back with the state backed up before. It warns when the laptop's UI build, or the UI running on it, is
# older than the engine it deploys.
#
# usage: tools/deploy-mini.sh [--locked]     takes the Mini lock for the copy, unless --locked (the caller holds it)
#   MINI=user@host or a host name (default: tools/mini-host.sh), BUILD=dir (default build)
set -euo pipefail

. "$(dirname "$0")/mini-host.sh"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-$ROOT/build}
APP=$BUILD/Nocturne.app
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=10 "$MINI")

lock() {
    NOCTURNE_MINI=$MINI "$ROOT/tools/mini-lock.sh" "$@"
}

check_build() {
    [[ -d $APP ]] || { echo "deploy-mini: no $APP; build first" >&2; exit 1; }
    for f in "$APP/Contents/MacOS/nocturne" "$BUILD/nocturne_tests"; do
        lipo -archs "$f" | grep -q x86_64 || { echo "deploy-mini: $f has no x86_64" >&2; exit 1; }
    done
    codesign --verify --strict "$APP"
    if [[ $(codesign -dvv "$APP" 2>&1) != *"Authority=Nocturne Dev"* ]]; then
        echo "deploy-mini: warning: $APP is not signed with Nocturne Dev; the Mini will ask for the microphone again" >&2
    fi
}

build_id() {
    grep -a -o 'nocturne-build [0-9]*-[0-9a-f]*' "$1" 2> /dev/null | head -n 1 | cut -d' ' -f2 || true
}

# A UI older than the engine cannot follow its edits; say so before the show finds out.
check_ui() {
    local ui="$BUILD/Nocturne UI.app/Contents/MacOS/Nocturne UI" engine ui_id pid started
    engine=$(build_id "$APP/Contents/MacOS/nocturne")
    ui_id=$(build_id "$ui")
    if [[ $ui_id < $engine ]]; then
        echo "deploy-mini: warning: the laptop UI (${ui_id:-no build id}) is older than the engine deployed ($engine);" \
            "rebuild it: cmake --build $BUILD --target nocturne-ui" >&2
    fi
    pid=$(pgrep -x "Nocturne UI" | head -n 1) || return 0
    [[ $(ps -o comm= -p "$pid") -ef $ui ]] || return 0
    started=$(LC_ALL=C date -j -f '%a %b %e %T %Y' "$(LC_ALL=C ps -o lstart= -p "$pid")" +%s 2> /dev/null) || return 0
    if [[ -f $ui ]] && ((started < $(stat -f %m "$ui"))); then
        echo "deploy-mini: warning: the running Nocturne UI started before its last build; restart it" >&2
    fi
}

stage() {
    local dir=$1
    mkdir -p "$dir/tools" "$dir/tests" "$dir/incoming"
    cp -R "$APP" "$dir/incoming/"
    cp "$ROOT"/tools/*.sh "$dir/tools/"
    cp -R "$ROOT/tools/mini-checks" "$dir/mini-checks"
    cp "$BUILD/nocturne_tests" "$BUILD/nocturne-hap-encode" "$ROOT"/tests/demo_*.sh "$dir/tests/"
    cp -R "$ROOT/tests/golden" "$ROOT/tests/media" "$dir/tests/"
    local source=$dir/tests/source
    mkdir -p "$source/tests" "$source/src" "$source/tools" "$source/docs/research"
    cp -R "$ROOT/tests/djlink" "$source/tests/"
    cp -R "$ROOT/src/djlink" "$source/src/"
    cp -R "$ROOT/tools/djlink-sim" "$ROOT/tools/update-bundle.sh" "$source/tools/"
    cp "$ROOT/docs/research/classic-controls.json" "$source/docs/research/"
}

check_build
check_ui
held=${1:-}
[[ $held == --locked ]] || lock acquire deploy-mini
staging=$(mktemp -d)
cleanup() {
    command rm -rf "$staging"
    [[ $held == --locked ]] || lock release
}
trap cleanup EXIT
stage "$staging"

"${SSH[@]}" 'command rm -rf ~/Nocturne/tools ~/Nocturne/mini-checks ~/Nocturne/tests ~/Nocturne/incoming && mkdir -p ~/Nocturne'
COPYFILE_DISABLE=1 tar -C "$staging" --no-mac-metadata -cf - . | "${SSH[@]}" 'tar -C ~/Nocturne -xf -'
# the installed bundle's own steps, or the new one's on a Mini that has none yet
"${SSH[@]}" 's=~/Nocturne/Nocturne.app/Contents/Resources/update-bundle.sh
    [ -f "$s" ] || s=~/Nocturne/incoming/Nocturne.app/Contents/Resources/update-bundle.sh
    bash "$s" ~/Nocturne/incoming/Nocturne.app' ||
    { echo "deploy-mini: the update did not go through; see its reason above" >&2; exit 1; }
echo "deployed $(git -C "$ROOT" rev-parse --short HEAD) to $MINI:~/Nocturne"
