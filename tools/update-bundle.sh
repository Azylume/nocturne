#!/bin/bash
# Installs a new Nocturne.app on the venue machine, run on that machine. The one set of steps behind both
# tools/deploy-mini.sh (over ssh) and the engine's Update (nocturne-ctl machine update, the Machines tab):
#
#   update-bundle.sh NEW_APP
#
#   1. stops the autostart agent if it runs
#   2. backs the autosaved state up as STATE/backups/TIME-update.json
#   3. keeps the running bundle aside as VENUE/Nocturne.app.previous and moves NEW_APP to VENUE/Nocturne.app
#   4. starts the agent again and checks the engine: within HEALTH_TIMEOUT s (default 60) it answers, renders frames
#      and reports NEW_APP's build id, and for HEALTH_STABLE s more (default 10) the supervisor does not restart it
#   5. healthy: deletes the previous bundle. Otherwise stops the agent, puts the previous bundle and the state backup
#      back, starts it again and exits 1.
#
# Without a running agent it only swaps the bundles. The result goes to STATE/update.json (the engine reports it in its
# machine status) and every step to LOGS/update.log. VENUE (default ~/Nocturne), STATE, LOGS and PORT (default the
# agent's --port, else 7400) may be set; AGENT names a command taking running|stop|start in place of launchctl, VERIFY
# one that checks a bundle in place of codesign, and DELAY the seconds to wait before stopping the engine (the engine's
# Update sets it, so its answer reaches the client first).
set -euo pipefail

VENUE=${VENUE:-$HOME/Nocturne}
STATE=${STATE:-"$HOME/Library/Application Support/Nocturne/State"}
LOGS=${LOGS:-"$HOME/Library/Logs/Nocturne"}
HEALTH_TIMEOUT=${HEALTH_TIMEOUT:-60}
HEALTH_STABLE=${HEALTH_STABLE:-10}
PLIST="$HOME/Library/LaunchAgents/ai.azylume.nocturne.plist"
APP="$VENUE/Nocturne.app"
PREVIOUS="$VENUE/Nocturne.app.previous"
NEW_ID= OLD_ID= BACKUP=

say() {
    echo "$(date '+%Y-%m-%d %H:%M:%S') $*" | tee -a "$LOGS/update.log" >&2
}

build_id() {
    grep -a -o 'nocturne-build [0-9]*-[0-9a-f]*' "$1/Contents/MacOS/nocturne" 2> /dev/null | head -n 1 | cut -d' ' -f2 || true
}

json_string() {
    local s=${1//\\/\\\\}
    printf '"%s"' "${s//\"/\\\"}"
}

result() {
    local tmp="$STATE/update.json.tmp"
    printf '{"result": %s, "build": %s, "previous": %s, "reason": %s, "at": %s}\n' "$(json_string "$1")" \
        "$(json_string "$NEW_ID")" "$(json_string "$OLD_ID")" "$(json_string "${2:-}")" "$(date +%s)" > "$tmp"
    mv "$tmp" "$STATE/update.json"
    say "$1${2:+: $2}"
}

agent() {
    if [[ -n ${AGENT:-} ]]; then
        "$AGENT" "$1"
        return
    fi
    local job="gui/$(id -u)/ai.azylume.nocturne"
    case $1 in
        running) launchctl print "$job" > /dev/null 2>&1 ;;
        stop) launchctl bootout "$job" ;;
        start)
            # bootstrap fails with an I/O error while the old job is still winding down
            for _ in 1 2 3 4 5; do
                launchctl bootstrap "gui/$(id -u)" "$PLIST" 2> /dev/null && return
                sleep 2
            done
            return 1
            ;;
    esac
}

port() {
    if [[ -n ${PORT:-} ]]; then
        echo "$PORT"
        return
    fi
    local args
    args=$(plutil -extract ProgramArguments json -o - "$PLIST" 2> /dev/null || true)
    [[ $args =~ \"--port\",\"([0-9]+)\" ]] && echo "${BASH_REMATCH[1]}" || echo 7400
}

field() {
    plutil -extract "$1" raw -o - "$2" 2> /dev/null || true
}

# Why the engine is not healthy yet, or nothing: it answers, renders and runs build `$1`.
unhealthy() {
    local reply="$STATE/update-probe.json"
    if ! "$APP/Contents/MacOS/nocturne-ctl" --port "$(port)" machine 2> /dev/null | grep '"op":"status"' |
        tail -n 1 > "$reply" || [[ ! -s $reply ]]; then
        echo "does not answer"
        return
    fi
    local mode version fps
    mode=$(field mode "$reply")
    version=$(field version "$reply")
    fps=$(field render.fps "$reply")
    if [[ $mode != normal ]]; then
        echo "is in ${mode:-an unknown} mode"
    elif [[ $version != "$1" ]]; then
        echo "reports build ${version:-none}, not $1"
    elif [[ ! $fps =~ ^[0-9.]+$ ]] || [[ $fps =~ ^0*\.?0*$ ]]; then
        echo "renders no frames"
    fi
}

# Waits for the engine to be healthy on build `$1` and stay so; prints why not.
check() {
    local why= starts
    for ((i = 0; i < HEALTH_TIMEOUT; i++)); do
        why=$(unhealthy "$1")
        [[ -z $why || $why == reports* ]] && break
        sleep 1
    done
    if [[ -n $why ]]; then
        [[ $why == reports* ]] || why="$why within $HEALTH_TIMEOUT s"
        echo "the engine $why"
        return
    fi
    starts=$(field starts "$STATE/status.json")
    sleep "$HEALTH_STABLE"
    if [[ $(field starts "$STATE/status.json") != "$starts" ]]; then
        echo "the engine was restarted by the supervisor within $HEALTH_STABLE s"
        return
    fi
    why=$(unhealthy "$1")
    [[ -z $why ]] || echo "the engine $why after $HEALTH_STABLE s"
}

restore_state() {
    if [[ -n $BACKUP ]]; then
        cp "$BACKUP" "$STATE/composition.json.tmp"
        mv "$STATE/composition.json.tmp" "$STATE/composition.json"
    else
        command rm -f "$STATE/composition.json"
    fi
}

rollback() {
    local why=$1
    say "unhealthy: $why; rolling back to ${OLD_ID:-the previous bundle}"
    agent stop || true
    command rm -rf "$APP"
    mv "$PREVIOUS" "$APP"
    restore_state
    if ! agent start; then
        result "rolled back" "$why; the previous build did not start again"
        exit 1
    fi
    local again
    again=$(check "$OLD_ID")
    result "rolled back" "$why${again:+; the previous build is not healthy either: $again}"
    exit 1
}

main() {
    local new=${1:?usage: update-bundle.sh NEW_APP}
    mkdir -p "$STATE/backups" "$LOGS"
    [[ -d $new ]] || { result failed "no bundle at $new"; exit 1; }
    new=$(cd "$new" && pwd)
    NEW_ID=$(build_id "$new")
    OLD_ID=$(build_id "$APP")
    [[ -n $NEW_ID ]] || { result failed "$new has no Nocturne engine with a build id"; exit 1; }
    if ! ${VERIFY:-codesign --verify --strict} "$new"; then
        result failed "$new does not pass the signature check"
        exit 1
    fi
    say "updating $APP from ${OLD_ID:-nothing} to $NEW_ID"
    sleep "${DELAY:-0}"
    local running=
    if agent running; then
        running=1
        agent stop
    fi
    if [[ -f $STATE/composition.json ]]; then
        BACKUP="$STATE/backups/$(date +%Y%m%d-%H%M%S.000)-update.json"
        cp "$STATE/composition.json" "$BACKUP"
        say "state backed up as $(basename "$BACKUP")"
    fi
    command rm -rf "$PREVIOUS"
    [[ -d $APP ]] && mv "$APP" "$PREVIOUS"
    mv "$new" "$APP"
    if [[ -z $running ]]; then
        result installed "the autostart agent does not run; not started"
        return
    fi
    if [[ -z $OLD_ID ]]; then
        agent start || { result failed "the autostart agent did not start"; exit 1; }
        result installed "no previous bundle to roll back to; not checked"
        return
    fi
    agent start || rollback "the autostart agent did not start"
    local why
    why=$(check "$NEW_ID")
    [[ -z $why ]] || rollback "$why"
    command rm -rf "$PREVIOUS"
    result updated
}

main "$@"
