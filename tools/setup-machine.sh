#!/bin/bash
# One-time show machine setup, run ON the show machine (docs/show-machine.md, "Mac mini show setup").
# Lists every change first, asks, then applies; a second run changes nothing that is already set.
#
#   tools/setup-machine.sh [--dry-run] [--yes] [--bin DIR] [--audio-input NAME] [--sudo] [--reinstall-autostart]
#                          [-- ENGINE OPTIONS...]
#   tools/setup-machine.sh --undo [--dry-run] [--yes] [--sudo]
#
# Without --sudo it changes only the current user's settings: the autostart LaunchAgent (through tools/autostart.sh),
# screen saver off, crash dialogs off, hot corners off, and the ~/Nocturne/shows and media folders. --sudo adds the
# system-wide steps (pmset, restart after a freeze, automatic macOS updates off); sudo asks for the password itself.
# --dry-run prints the changes and the commands and changes nothing. --undo puts back the values the first run saw
# (kept in ~/Library/Application Support/Nocturne/setup-machine-undo.txt) and removes the LaunchAgent.
# DIR holds nocturne and nocturne-supervisor (default: ~/Nocturne/Nocturne.app, then /Applications/Nocturne.app).
# Nothing is downloaded and no secret is kept. Not touched: FileVault, automatic login, Focus, Spotlight, permissions.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
UNDO="$HOME/Library/Application Support/Nocturne/setup-machine-undo.txt"
SU=/Library/Preferences/com.apple.SoftwareUpdate
PMSET_KEYS="sleep=0 displaysleep=0 disksleep=0 powernap=0 autorestart=1"

dry=0 yes=0 undo=0 sudo=0 reinstall=0 bin='' audio=''
engine_opts=()
planned=0 dock_dirty=0 changed=0
mode=plan

die() { echo "setup-machine: $*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
    case $1 in
        --dry-run) dry=1 ;;
        --yes) yes=1 ;;
        --undo) undo=1 ;;
        --sudo) sudo=1 ;;
        --reinstall-autostart) reinstall=1 ;;
        --bin) [[ $# -ge 2 ]] || die "--bin needs a folder"; bin=$2; shift ;;
        --audio-input) [[ $# -ge 2 ]] || die "--audio-input needs a device name"; audio=$2; shift ;;
        --) shift; engine_opts=("$@"); break ;;
        -h | --help) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
[[ -z $audio ]] || engine_opts=(--audio-input "$audio" ${engine_opts[@]+"${engine_opts[@]}"})

# Runs CMD in apply mode; in plan mode lists it.
change() {
    local desc=$1
    shift
    if [[ $mode == plan ]]; then
        planned=$((planned + 1))
        printf '  - %s\n      $ %s\n' "$desc" "$*"
        return
    fi
    echo "  $desc"
    "$@"
}

remember() {
    grep -qF -- "$1" "$UNDO" 2> /dev/null && return 0
    mkdir -p "$(dirname "$UNDO")"
    printf '%s\n' "$1" >> "$UNDO"
}

# set_default HOST DOMAIN KEY TYPE WANT [OK_PATTERN]: HOST is -currentHost or -; a DOMAIN path is system-wide (sudo).
set_default() {
    local host=$1 domain=$2 key=$3 type=$4 want=$5 ok=${6:-}
    local flags=() pre=()
    [[ $host == -currentHost ]] && flags=(-currentHost)
    [[ $domain == /* ]] && pre=(sudo)
    local cur wanted=$want old
    changed=0
    cur=$(defaults ${flags[@]+"${flags[@]}"} read "$domain" "$key" 2> /dev/null || echo -)
    [[ $type == bool ]] && wanted=$([[ $want == true ]] && echo 1 || echo 0)
    if [[ $cur == "$wanted" || (-n $ok && $cur =~ $ok) ]]; then
        return 0
    fi
    changed=1
    old=$cur
    [[ $type == bool && $cur != - ]] && old=$([[ $cur == 1 ]] && echo true || echo false)
    [[ $mode == apply ]] && remember "defaults|$host|$domain|$key|$type|$old"
    change "$domain $key: ${cur/#-/unset} -> $want" ${pre[@]+"${pre[@]}"} defaults ${flags[@]+"${flags[@]}"} write "$domain" "$key" "-$type" "$want"
}

check_prerequisites() {
    [[ $(uname) == Darwin ]] || die "macOS only (a Windows show machine uses installer/autostart.ps1)"
    [[ $(id -u) != 0 ]] || die "run it as the show user, not root"
    local major
    major=$(sw_vers -productVersion | cut -d. -f1)
    ((major >= 14)) || die "macOS 14 or newer needed (this is $(sw_vers -productVersion))"
    [[ $undo == 1 ]] && return 0
    if [[ -z $bin ]]; then
        for app in "$HOME/Nocturne/Nocturne.app" /Applications/Nocturne.app; do
            [[ -x $app/Contents/MacOS/nocturne-supervisor ]] && { bin=$app/Contents/MacOS; break; }
        done
    fi
    [[ -n $bin ]] || die "no Nocturne.app found in ~/Nocturne or /Applications; install it first (tools/deploy-mini.sh, or the DMG) or pass --bin DIR"
    [[ -d $bin ]] || die "no folder $bin"
    bin=$(cd "$bin" && pwd)
    for exe in nocturne nocturne-supervisor; do
        [[ -x $bin/$exe ]] || die "no $bin/$exe"
    done
    [[ $(stat -f %Su /dev/console) == "$(id -un)" ]] || echo "note: $(id -un) does not own the desktop session now; the LaunchAgent runs only there" >&2
}

user_steps() {
    local plist=$HOME/Library/LaunchAgents/ai.azylume.nocturne.plist
    if [[ -f $plist && $reinstall == 0 ]]; then
        echo "  (autostart already installed: kept; --reinstall-autostart rewrites it and restarts the engine)"
    else
        change "autostart LaunchAgent, engine options: ${engine_opts[*]:-none}" "$HERE/autostart.sh" install --bin "$bin" -- ${engine_opts[@]+"${engine_opts[@]}"}
    fi
    for d in shows media; do
        [[ -d $HOME/Nocturne/$d ]] || change "folder ~/Nocturne/$d" mkdir -p "$HOME/Nocturne/$d"
    done
    set_default -currentHost com.apple.screensaver idleTime int 0
    set_default - com.apple.CrashReporter DialogType string none
    local c
    for c in tl tr bl br; do
        set_default - com.apple.dock "wvous-$c-corner" int 1 '^[-01]$'
        [[ $changed == 0 ]] || dock_dirty=1
    done
    [[ $dock_dirty == 0 ]] || change "restart the Dock to apply the hot corners" killall Dock
}

sudo_steps() {
    local pm kv k want got
    pm=$(pmset -g)
    for kv in $PMSET_KEYS; do
        k=${kv%=*} want=${kv#*=}
        got=$(awk -v k="$k" '$1 == k { print $2; exit }' <<< "$pm")
        [[ -z $got && $k == powernap ]] && continue
        [[ $got == "$want" ]] && continue
        [[ $mode == apply ]] && remember "pmset|$k|${got:--}"
        change "pmset $k: ${got:-unset} -> $want" sudo pmset -a "$k" "$want"
    done
    local freeze
    freeze=$(sudo -n systemsetup -getrestartfreeze 2> /dev/null || true)
    [[ $freeze == *": On"* ]] && return 0
    [[ $mode == apply && $freeze == *": Off"* ]] && remember "freeze|off"
    change "restart after a freeze: on (now: ${freeze:+${freeze##*: }}${freeze:-unreadable without sudo})" sudo systemsetup -setrestartfreeze on
    set_default - $SU AutomaticDownload bool false
    set_default - $SU AutomaticallyInstallMacOSUpdates bool false
}

all_steps() {
    echo "Settings of $(id -un):"
    user_steps
    if [[ $sudo == 1 ]]; then
        echo "System-wide (sudo):"
        sudo_steps
    else
        echo "System-wide steps skipped (no --sudo)."
    fi
}

undo_defaults() {
    local host=$1 domain=$2 key=$3 type=$4 old=$5
    local flags=() pre=()
    [[ $host == -currentHost ]] && flags=(-currentHost)
    if [[ $domain == /* ]]; then
        [[ $sudo == 1 ]] || { echo "  (skipped $domain $key: needs --sudo)"; return 0; }
        pre=(sudo)
    fi
    if [[ $old != - ]]; then
        change "$domain $key: back to $old" ${pre[@]+"${pre[@]}"} defaults ${flags[@]+"${flags[@]}"} write "$domain" "$key" "-$type" "$old"
    elif defaults ${flags[@]+"${flags[@]}"} read "$domain" "$key" > /dev/null 2>&1; then
        change "$domain $key: delete (was unset)" ${pre[@]+"${pre[@]}"} defaults ${flags[@]+"${flags[@]}"} delete "$domain" "$key"
    fi
}

undo_steps() {
    local kind a b c d e
    if [[ -f $UNDO ]]; then
        while IFS='|' read -r kind a b c d e; do
            case $kind in
                defaults) undo_defaults "$a" "$b" "$c" "$d" "$e" ;;
                freeze)
                    [[ $sudo == 1 ]] || { echo "  (skipped restart after a freeze: needs --sudo)"; continue; }
                    change "restart after a freeze: off (was off)" sudo systemsetup -setrestartfreeze off ;;
                pmset)
                    [[ $sudo == 1 ]] || { echo "  (skipped pmset $a: needs --sudo)"; continue; }
                    [[ $b == - ]] || change "pmset $a back to $b" sudo pmset -a "$a" "$b" ;;
            esac
        done < "$UNDO"
    else
        echo "  (no $UNDO: no saved settings to restore)"
    fi
    grep -q '|com.apple.dock|' "$UNDO" 2> /dev/null && change "restart the Dock to apply the hot corners" killall Dock
    change "autostart LaunchAgent removed" "$HERE/autostart.sh" uninstall
}

checklist() {
    cat <<EOF

Still for a human (this script cannot do these):
  - Microphone: $bin/nocturne --mic-check, click Allow once (then check engine.log for a live audio level).
  - Local Network: allow Nocturne when macOS asks, on the first run with a second machine.
  - Pairing: pair the UI or nocturne-ctl from another machine once the engine runs.
  - Outputs: nocturne-ctl screendisplay SCREEN DISPLAY (ids from: $bin/nocturne --displays).
  - Automatic login (needs FileVault off), a Focus that silences notifications, Spotlight Search Privacy for ~/Nocturne/media.
EOF
    [[ $sudo == 1 ]] || echo "  - Not done without --sudo: sleep off, restart after a power cut or freeze, automatic macOS updates off (see docs/show-machine.md)."
    echo "Check it: tools/mini-setup-check.sh. Undo it: $0 --undo."
}

confirm() {
    [[ $yes == 1 ]] && return 0
    [[ -t 0 ]] || die "not a terminal: pass --yes to apply without asking"
    local reply
    read -r -p "Apply these $planned changes? [y/N] " reply
    [[ $reply == [yY] || $reply == yes ]] || die "nothing changed"
}

main() {
    check_prerequisites
    local steps=all_steps
    [[ $undo == 1 ]] && steps=undo_steps
    echo "Changes to make$([[ $dry == 1 ]] && echo " (dry run)"):"
    $steps
    if [[ $dry == 1 ]]; then
        echo "Dry run: nothing was changed."
        return 0
    fi
    [[ $planned -gt 0 ]] || { echo "Nothing to change."; [[ $undo == 1 ]] || checklist; return 0; }
    confirm
    mode=apply
    $steps
    [[ $undo == 1 ]] && command rm -f "$UNDO"
    echo "Done."
    [[ $undo == 1 ]] || checklist
}

main
