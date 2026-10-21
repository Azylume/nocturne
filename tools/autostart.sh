#!/bin/bash
# Starts Nocturne at login under its supervisor, as a LaunchAgent in the user's desktop session.
#
#   tools/autostart.sh install [--bin DIR] [-- ENGINE OPTIONS...]
#   tools/autostart.sh uninstall | status | restart | logs
#
# install writes ~/Library/LaunchAgents/ai.azylume.nocturne.plist and loads it at once. DIR holds nocturne and
# nocturne-supervisor (default: ~/Nocturne/Nocturne.app/Contents/MacOS, where tools/deploy-mini.sh installs). ENGINE OPTIONS go to every engine start, e.g.
# --audio-input Focusrite --fps 60; the audio analysis is always logged to ~/Library/Logs/Nocturne/audio.jsonl.
# restart restarts the supervisor and the engine, which also leaves safe mode.
set -euo pipefail

LABEL=ai.azylume.nocturne
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOGS="$HOME/Library/Logs/Nocturne"
STATE="$HOME/Library/Application Support/Nocturne/State"
DOMAIN="gui/$(id -u)"

xml() {
    printf '%s' "$1" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g'
}

install() {
    local bin
    bin="$HOME/Nocturne/Nocturne.app/Contents/MacOS"
    if [[ ${1:-} == --bin ]]; then
        bin=$(cd "$2" && pwd)
        shift 2
    fi
    [[ ${1:-} == -- ]] && shift
    for exe in nocturne nocturne-supervisor; do
        [[ -x "$bin/$exe" ]] || { echo "no $bin/$exe" >&2; exit 1; }
    done
    mkdir -p "$LOGS" "$STATE" "$(dirname "$PLIST")"

    local args=("$bin/nocturne-supervisor" --state "$STATE" --logs "$LOGS" -- --audio-log "$LOGS/audio.jsonl" --stats-json "$LOGS/stats.jsonl" "$@")
    {
        cat <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>$LABEL</string>
    <key>ProgramArguments</key>
    <array>
EOF
        for a in "${args[@]}"; do
            printf '        <string>%s</string>\n' "$(xml "$a")"
        done
        cat <<EOF
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>ThrottleInterval</key>
    <integer>5</integer>
    <key>ProcessType</key>
    <string>Interactive</string>
    <key>LimitLoadToSessionType</key>
    <string>Aqua</string>
    <key>StandardOutPath</key>
    <string>$(xml "$LOGS/launchd.log")</string>
    <key>StandardErrorPath</key>
    <string>$(xml "$LOGS/launchd.log")</string>
</dict>
</plist>
EOF
    } > "$PLIST"
    plutil -lint "$PLIST" > /dev/null

    launchctl bootout "$DOMAIN/$LABEL" 2> /dev/null || true
    launchctl enable "$DOMAIN/$LABEL"
    # bootstrap fails with an I/O error while the old job is still winding down
    for i in 1 2 3 4 5; do
        launchctl bootstrap "$DOMAIN" "$PLIST" 2> /dev/null && break
        ((i == 5)) && { echo "autostart: cannot start $LABEL" >&2; exit 1; }
        sleep 2
    done
    echo "installed $PLIST; Nocturne starts now and at every login"
    echo "logs: $LOGS"
}

uninstall() {
    launchctl bootout "$DOMAIN/$LABEL" 2> /dev/null || true
    rm -f "$PLIST"
    echo "uninstalled; the engine no longer starts at login"
}

status() {
    if launchctl print "$DOMAIN/$LABEL" > /dev/null 2>&1; then
        launchctl print "$DOMAIN/$LABEL" | grep -E '^\s*(state|pid|last exit code|runs) =' || true
    else
        echo "not loaded"
    fi
    [[ -f "$STATE/status.json" ]] && cat "$STATE/status.json"
    return 0
}

case "${1:-}" in
    install) shift; install "$@" ;;
    uninstall) uninstall ;;
    status) status ;;
    restart) launchctl kickstart -k "$DOMAIN/$LABEL" && echo "restarted" ;;
    logs) tail -n 40 "$LOGS/supervisor.log" ;;
    *) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
