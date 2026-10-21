#!/bin/bash
# Lists and restores the backups the engine keeps of its autosaved state, on the machine the engine runs on (the Mac
# mini: ~/Nocturne/tools/state-backup.sh). Backups are in STATE/backups: TIME-minute.json once a minute while the show
# changes, TIME-load.json before a show file replaces it, TIME-restore.json before a restore; the last 20 of each are kept.
#
#   tools/state-backup.sh list               the backups, oldest first, with the show each holds
#   tools/state-backup.sh restore NAME       stops the autostart agent, makes backup NAME the state, starts it again
#
# A restored backup of another show than the autostart's --composition is replaced by that show at start (backed up
# again as -load); reinstall the autostart on its show first. STATE defaults to ~/Library/Application Support/Nocturne/State.
set -euo pipefail

STATE=${STATE:-"$HOME/Library/Application Support/Nocturne/State"}
BACKUPS="$STATE/backups"
PLIST="$HOME/Library/LaunchAgents/ai.azylume.nocturne.plist"
AGENT="gui/$(id -u)/ai.azylume.nocturne"

show_of() {
    plutil -extract show raw -o - "$1" 2> /dev/null || echo "-"
}

list() {
    [[ -d $BACKUPS ]] || { echo "no backups in $BACKUPS"; return; }
    for f in "$BACKUPS"/*.json; do
        [[ -f $f ]] && printf '%s  %s\n' "$(basename "$f")" "$(show_of "$f")"
    done
}

restore() {
    local backup="$BACKUPS/${1:?usage: tools/state-backup.sh restore NAME}"
    [[ -f $backup ]] || { echo "state-backup: no $backup" >&2; exit 1; }
    local running=
    if launchctl print "$AGENT" > /dev/null 2>&1; then
        running=1
        launchctl bootout "$AGENT"
    elif pgrep -x nocturne > /dev/null; then
        echo "state-backup: an engine runs outside the autostart agent; quit it first" >&2
        exit 1
    fi
    [[ -f $STATE/composition.json ]] && cp "$STATE/composition.json" "$BACKUPS/$(date +%Y%m%d-%H%M%S.000)-restore.json"
    cp "$backup" "$STATE/composition.json.tmp"
    mv "$STATE/composition.json.tmp" "$STATE/composition.json"
    echo "restored $(basename "$backup") ($(show_of "$backup"))"
    if [[ -f $PLIST ]] && ! grep -qF "$(show_of "$backup")" "$PLIST"; then
        echo "state-backup: warning: the autostart starts another show, which replaces this state" >&2
    fi
    [[ -z $running ]] && return
    # bootstrap fails with an I/O error while the old job is still winding down
    for i in 1 2 3 4 5; do
        launchctl bootstrap "gui/$(id -u)" "$PLIST" 2> /dev/null && { echo "autostart agent started again"; return; }
        sleep 2
    done
    echo "state-backup: cannot start the autostart agent" >&2
    exit 1
}

case "${1:-}" in
    list) list ;;
    restore) shift; restore "$@" ;;
    *) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
