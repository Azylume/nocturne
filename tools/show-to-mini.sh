#!/bin/bash
# Takes a show from the laptop to the Mac mini and proves it plays there:
#   1. build (Universal, nice 15)            4. autostart install with --composition (restarts the engine on it)
#   2. tools/deploy-mini.sh                   5. verify over the LAN with nocturne-ctl: the beat and the clips advance
#   3. tools/sync-show.sh SHOW.json
# Steps 2-4 hold the Mini lock once. --dry-run prints every step and runs nothing. The engine keeps its autosaved state
# (the live edits) when it holds this show, even if SHOW.json changed; it loads the file only for another show or when
# asked, and the run says which. Every load backs the state up first (tools/state-backup.sh lists and restores them).
#
# usage: tools/show-to-mini.sh [--dry-run] [--no-build] [--reload] [--remap-displays] [--node | --follow MAIN] SHOW.json
#                              [MEDIA_ROOT] [-- ENGINE OPTIONS]
#   --reload                 once the engine runs, loads the show file, replacing the live state
#   --node / --follow MAIN   the Mini follows the laptop's engine (Bonjour, or HOST[:PORT]) when it is up, and plays the
#                            show on its own clock when it is not
#   --remap-displays         --reload, moving screens whose displays are not attached to free ones, even displays the
#                            Mini has seen before (by default those stay parked)
#   MINI=user@host or a host name (default: tools/mini-host.sh), AUDIO_INPUT (default Focusrite), PORT (default 7400),
#   NOCTURNE_TOKEN (the Mini's pairing token for the LAN check; default: read over ssh)
set -euo pipefail

. "$(dirname "$0")/mini-host.sh"
AUDIO_INPUT=${AUDIO_INPUT:-Focusrite}
PORT=${PORT:-7400}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=$ROOT/build
CTL=$BUILD/Nocturne.app/Contents/MacOS/nocturne-ctl
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=10 "$MINI")

dry= build=1 reload= remap= follow=() file= media_root= engine=()
while (($#)); do
    case $1 in
        --dry-run) dry=1 ;;
        --no-build) build= ;;
        --reload) reload=1 ;;
        --remap-displays) reload=1 remap=1 ;;
        --node) follow=(--node) ;;
        --follow) follow=(--follow "$2"); shift ;;
        --) shift; engine=("$@"); break ;;
        -*) echo "show-to-mini: unknown option $1" >&2; exit 2 ;;
        *) if [[ -z $file ]]; then file=$1; else media_root=$1; fi ;;
    esac
    shift
done
[[ -n $file ]] || { sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
[[ -n $dry || -f $file ]] || { echo "show-to-mini: no $file" >&2; exit 1; }

show=$(basename "$file")
remote_show="\$HOME/Nocturne/shows/$(printf %q "$show")"

step() { printf '\n== %s\n' "$*"; }
run() {
    printf '+'
    printf ' %q' "$@"
    printf '\n'
    [[ -n $dry ]] || "$@"
}
on_mini() {
    printf '+ ssh %s %s\n' "$MINI" "$1"
    [[ -n $dry ]] || "${SSH[@]}" "$1"
}

release() {
    run env NOCTURNE_MINI="$MINI" "$ROOT/tools/mini-lock.sh" release
}

autostart_args() {
    local a=(--audio-input "$AUDIO_INPUT" ${follow[@]+"${follow[@]}"} ${engine[@]+"${engine[@]}"})
    printf ' %q' "${a[@]}"
    printf ' --composition %s --media-root "$HOME/Nocturne/media"' "$remote_show"
}

# Two clock readings 2 s apart: the beat must move, and a show with clips must have a clip frame moving.
playing() {
    local first second
    first=$("$@" clock 2> /dev/null | grep '"type":"clock"') || return 1
    sleep 2
    second=$("$@" clock 2> /dev/null | grep '"type":"clock"') || return 1
    python3 - "$first" "$second" <<'EOF'
import json, sys
a, b = (json.loads(s) for s in sys.argv[1:3])
frames = lambda c: {(x["layer"], x.get("column")): x["frame"] for x in c["clips"]}
moved = b["beat"] > a["beat"] and (not b["clips"] or frames(a) != frames(b))
print(f"beat {a['beat']:.2f} -> {b['beat']:.2f} at {b['tempo']:.1f} BPM, {len(b['clips'])} clips playing")
sys.exit(0 if moved else 1)
EOF
}

reload_show() {
    local ctl="~/Nocturne/Nocturne.app/Contents/MacOS/nocturne-ctl --port $PORT"
    local load="$ctl load $remote_show \"\$HOME/Nocturne/media\"${remap:+ --remap-displays}"
    step "reload: load the show file on the Mini, replacing the live state${remap:+, screens moving to free displays}"
    if [[ -z $dry ]]; then
        local deadline=$((SECONDS + 45))
        until "${SSH[@]}" "$ctl --json clock" > /dev/null 2>&1; do
            ((SECONDS < deadline)) || { echo "show-to-mini: FAIL: no engine on the Mini to load the show into" >&2; exit 1; }
            sleep 3
        done
    fi
    on_mini "$load | grep -o '\"warnings\".*'"
}

verify() {
    local host=${MINI#*@}
    step "verify: the engine on $host plays"
    if [[ -n $dry ]]; then
        printf '+ %q --host %q --port %s --json clock   (twice, 2 s apart; beat and clip frames must advance)\n' "$CTL" "$host" "$PORT"
        on_mini "~/Nocturne/tools/autostart.sh status; grep -E 'composition: (loaded|warning)|continuing the saved state' ~/Library/Logs/Nocturne/engine.log | tail -n 20"
        return
    fi
    # The engine lets in clients from other machines only with its pairing token.
    [[ -n ${NOCTURNE_TOKEN:-} ]] ||
        NOCTURNE_TOKEN=$("${SSH[@]}" "~/Nocturne/Nocturne.app/Contents/MacOS/nocturne-ctl token" 2> /dev/null || true)
    export NOCTURNE_TOKEN
    local deadline=$((SECONDS + 45))
    until playing "$CTL" --host "$host" --port "$PORT" --json; do
        if ((SECONDS > deadline)); then
            echo "show-to-mini: no playing engine over the LAN after 45 s; trying over ssh on the Mini's loopback" >&2
            echo "  (if this works, the laptop lacks the Local Network grant for Nocturne, or WARP captures the LAN route)" >&2
            playing "${SSH[@]}" "~/Nocturne/Nocturne.app/Contents/MacOS/nocturne-ctl --port $PORT --json" ||
                { echo "show-to-mini: FAIL: the engine is not playing" >&2; on_mini "~/Nocturne/tools/autostart.sh status; ~/Nocturne/tools/autostart.sh logs"; exit 1; }
            break
        fi
        sleep 3
    done
    on_mini "~/Nocturne/tools/autostart.sh status"
    local log
    log=$("${SSH[@]}" "grep -E 'composition: (loaded|warning)|continuing the saved state' ~/Library/Logs/Nocturne/engine.log | tail -n 20" || true)
    printf '%s\n' "$log"
    if grep -q 'is not attached' <<< "$log"; then
        echo "show-to-mini: WARNING: a screen's display was not attached when the show loaded" >&2
    fi
    if [[ -n $reload ]]; then
        echo "show-to-mini: loaded $show from the file as asked; the live state before it is backed up"
    elif [[ $(grep -E 'composition: loaded|continuing the saved state' <<< "$log" | tail -n 1) == *continuing* ]]; then
        echo "show-to-mini: kept the live state of $show (--reload loads the file)"
    else
        echo "show-to-mini: loaded $show from the file: the state held another show (backed up)"
    fi
    echo "show-to-mini: PASS: $show plays on $MINI"
}

if [[ -n $build ]]; then
    step "build"
    [[ -f $BUILD/CMakeCache.txt ]] || run nice -n 15 cmake -S "$ROOT" -B "$BUILD"
    run nice -n 15 cmake --build "$BUILD" -j8
fi

step "lock the Mini"
run env NOCTURNE_MINI="$MINI" "$ROOT/tools/mini-lock.sh" acquire show-to-mini
[[ -n $dry ]] || trap release EXIT

step "deploy the build"
run env MINI="$MINI" BUILD="$BUILD" "$ROOT/tools/deploy-mini.sh" --locked

step "sync the show and its media"
run env MINI="$MINI" CTL="$CTL" "$ROOT/tools/sync-show.sh" --locked "$file" ${media_root:+"$media_root"}

step "autostart on the show (install reloads the agent: the engine restarts on it)"
on_mini "~/Nocturne/tools/autostart.sh install --$(autostart_args)"

step "unlock the Mini"
trap - EXIT
release

[[ -z $reload ]] || reload_show

verify
