#!/bin/bash
# Checks the Mac mini's one-time show setup (README "Mac mini show setup") from the laptop over SSH, changing nothing.
# Prints PASS, FAIL (with the exact fix) or MANUAL (cannot be read without sudo or a GUI; what to check and how) per item,
# and exits 1 when anything fails.
#
# usage: tools/mini-setup-check.sh [--locked]     takes the Mini lock (the checks start the engine binary briefly)
#   MINI=user@host or a host name (default: tools/mini-host.sh); MINI=local runs the checks on this machine
set -euo pipefail

. "$(dirname "$0")/mini-host.sh"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=10 "$MINI")

remote() {
    if [[ $MINI == local ]]; then
        bash -s
    else
        "${SSH[@]}" bash -s
    fi
}

checks() {
    remote <<'REMOTE'
APP=~/Nocturne/Nocturne.app
EXE=$APP/Contents/MacOS/nocturne
PLIST=~/Library/LaunchAgents/ai.azylume.nocturne.plist
MEDIA=~/Nocturne/media
LOGS=~/Library/Logs/Nocturne
ME=$(id -un)

pass() { printf 'PASS    %s\n' "$1"; }
fail() { printf 'FAIL    %s\n        fix: %s\n' "$1" "$2"; }
manual() { printf 'MANUAL  %s\n        do: %s\n' "$1" "$2"; }
within() { perl -e 'alarm shift; exec @ARGV' "$@"; }

[[ $(fdesetup status 2> /dev/null) == *"FileVault is Off"* ]] && pass "FileVault off" ||
    fail "FileVault off ($(fdesetup status 2>&1 | head -1))" "System Settings > Privacy & Security > FileVault > Turn Off (or sudo fdesetup disable); wait for decryption to finish"

auto=$(defaults read /Library/Preferences/com.apple.loginwindow autoLoginUser 2> /dev/null || true)
if [[ $auto == "$ME" && -e /etc/kcpassword ]]; then
    pass "automatic login as $ME"
else
    fail "automatic login as $ME (now: ${auto:-off})" "System Settings > Users & Groups > Automatically log in as: $ME (enter the password; needs FileVault off)"
fi

[[ $(stat -f %Su /dev/console) == "$ME" ]] && pass "$ME owns the desktop session" ||
    fail "$ME owns the desktop session (now: $(stat -f %Su /dev/console))" "log in as $ME on the Mini's screen; the LaunchAgent runs only in that session"

pm=$(pmset -g)
pmval() { awk -v k="$1" '$1 == k { print $2; exit }' <<< "$pm"; }
for kv in sleep=0 displaysleep=0 disksleep=0 powernap=0 autorestart=1; do
    k=${kv%=*} want=${kv#*=} got=$(pmval "${kv%=*}")
    if [[ -z $got && $k == powernap ]]; then
        pass "pmset powernap (not offered)"
    elif [[ $got == "$want" ]]; then
        pass "pmset $k $want"
    else
        fail "pmset $k $want (now: ${got:-unset})" "sudo pmset -a sleep 0 displaysleep 0 disksleep 0 powernap 0 autorestart 1"
    fi
done

freeze=$(sudo -n systemsetup -getrestartfreeze 2> /dev/null || true)
if [[ $freeze == *": On"* ]]; then
    pass "restart after a freeze"
elif [[ -n $freeze ]]; then
    fail "restart after a freeze ($freeze)" "sudo systemsetup -setrestartfreeze on"
else
    manual "restart after a freeze (needs sudo to read)" "on the Mini: sudo systemsetup -getrestartfreeze (expect On); if Off: sudo systemsetup -setrestartfreeze on"
fi

idle=$(defaults -currentHost read com.apple.screensaver idleTime 2> /dev/null || echo unset)
[[ $idle == 0 ]] && pass "screen saver never" ||
    fail "screen saver never (idleTime: $idle)" "defaults -currentHost write com.apple.screensaver idleTime 0; and System Settings > Lock Screen > Start Screen Saver when inactive: Never"

lock=$(within 5 sysadminctl -screenLock status 2>&1 || true)
if [[ $lock == *"screenLock is off"* ]]; then
    pass "screen lock off"
elif [[ $lock == *"screenLock"* ]]; then
    fail "screen lock off (${lock##*] })" "System Settings > Lock Screen > Require password after screen saver begins or display is turned off: Never"
else
    manual "screen lock off (not readable here)" "System Settings > Lock Screen > Require password ...: Never"
fi

dialog=$(defaults read com.apple.CrashReporter DialogType 2> /dev/null || echo unset)
[[ $dialog == none ]] && pass "crash dialogs off" ||
    fail "crash dialogs off (DialogType: $dialog)" "defaults write com.apple.CrashReporter DialogType none"

corners=
for c in tl tr bl br; do
    v=$(defaults read com.apple.dock "wvous-$c-corner" 2> /dev/null || echo 1)
    [[ $v == 0 || $v == 1 ]] || corners+=" $c=$v"
done
[[ -z $corners ]] && pass "hot corners off" ||
    fail "hot corners off (set:$corners)" "for c in tl tr bl br; do defaults write com.apple.dock wvous-\$c-corner -int 1; done; killall Dock"

files=$(find "$MEDIA" -type f ! -name '.*' 2> /dev/null | wc -l | tr -d ' ')
indexed=$(mdfind -onlyin "$MEDIA" 'kMDItemFSName == "*"' 2> /dev/null | wc -l | tr -d ' ')
if [[ ! -d $MEDIA || $files == 0 ]]; then
    manual "Spotlight skips $MEDIA (empty, cannot tell yet)" "System Settings > Spotlight > Search Privacy > + $MEDIA; rerun after tools/sync-show.sh"
elif [[ $indexed == 0 ]]; then
    pass "Spotlight skips $MEDIA ($files files, none indexed)"
else
    fail "Spotlight skips $MEDIA ($indexed of $files files indexed)" "System Settings > Spotlight > Search Privacy > + $MEDIA (mkdir -p it first)"
fi
mad=$(ps -Ao pcpu=,comm= | awk '/mediaanalysisd/ { s += $1 } END { printf "%d", s }')
[[ $mad -lt 20 ]] && pass "mediaanalysisd idle (${mad}% CPU)" ||
    fail "mediaanalysisd idle (${mad}% CPU)" "exclude $MEDIA from Spotlight as above and let it finish; never copy media during a show"

focus=$(plutil -extract data.0.storeAssertionRecords raw ~/Library/DoNotDisturb/DB/Assertions.json 2> /dev/null || true)
if [[ $focus =~ ^[1-9] ]]; then
    pass "a Focus silences notifications"
else
    manual "a Focus silences notifications (not readable here)" "Control Centre > Focus > Do Not Disturb on, kept on (Focus > Do Not Disturb > Turn On: until turned off); or System Settings > Notifications: off per app"
fi

su=/Library/Preferences/com.apple.SoftwareUpdate
updates=
for k in AutomaticDownload AutomaticallyInstallMacOSUpdates; do
    [[ $(defaults read $su $k 2> /dev/null || echo unset) == 0 ]] || updates+=" $k"
done
[[ -z $updates ]] && pass "automatic macOS updates off" ||
    fail "automatic macOS updates off (on or unset:$updates)" "System Settings > General > Software Update > Automatic Updates: all off (or sudo defaults write $su AutomaticDownload -bool false; sudo defaults write $su AutomaticallyInstallMacOSUpdates -bool false)"

others=$(ls /Library/LaunchAgents /Library/LaunchDaemons ~/Library/LaunchAgents 2> /dev/null | grep -v -e '^/' -e '^$' -e '^com\.apple\.' -e '^ai\.azylume\.' | tr '\n' ' ' || true)
[[ -z $others ]] && pass "no other launch agents or daemons" ||
    fail "no other launch agents or daemons (found: $others)" "remove what is not needed for the show: sudo launchctl bootout system/LABEL; sudo rm /Library/LaunchDaemons/LABEL.plist (or the LaunchAgents path)"

if security find-certificate -c mkcert /Library/Keychains/System.keychain > /dev/null 2>&1; then
    fail "no leftover mkcert root CA" "sudo security delete-certificate -c mkcert /Library/Keychains/System.keychain"
else
    pass "no leftover mkcert root CA"
fi

if [[ ! -x $EXE ]]; then
    fail "Nocturne installed at $APP" "on the laptop: tools/deploy-mini.sh"
else
    pass "Nocturne installed at $APP"
    sig=$(codesign -dvv "$APP" 2>&1 || true)
    if codesign --verify --strict "$APP" 2> /dev/null && [[ $sig == *"Authority=Nocturne Dev"* ]]; then
        pass "bundle signed with Nocturne Dev"
    else
        fail "bundle signed with Nocturne Dev" "on the laptop: tools/make-signing-identity.sh (once), rebuild, tools/deploy-mini.sh"
    fi

    tcc=$(sqlite3 ~/Library/Application\ Support/com.apple.TCC/TCC.db \
        "select auth_value from access where service = 'kTCCServiceMicrophone' and client = 'com.azylume.nocturne'" 2> /dev/null || echo unreadable)
    case $tcc in
        2) pass "microphone granted (TCC)" ;;
        0) fail "microphone granted (TCC: denied)" "tccutil reset Microphone com.azylume.nocturne; then on the Mini's screen: open -W $APP --args --mic-check and click Allow" ;;
        *)
            mic=$(within 15 "$EXE" --mic-check 2>&1 | head -1 || true)
            case $mic in
                *": authorized") pass "microphone granted (--mic-check)" ;;
                *": notDetermined") fail "microphone granted (never asked; the Mini's screen may show the prompt now)" "in the desktop session: open -W $APP --args --mic-check and click Allow" ;;
                *": denied") fail "microphone granted (denied)" "tccutil reset Microphone com.azylume.nocturne; then open -W $APP --args --mic-check and click Allow" ;;
                *) manual "microphone granted (--mic-check said: ${mic:-nothing})" "open -W $APP --args --mic-check on the Mini; expect authorized" ;;
            esac
            ;;
    esac

    echo "INFO    displays: $(within 15 "$EXE" --displays 2> /dev/null | grep '^display' | tr '\n' ';' || true)"
fi

if [[ -f $LOGS/engine.log ]]; then
    bonjour=$(grep 'bonjour:' "$LOGS/engine.log" | tail -1 || true)
    if [[ $bonjour == *"access denied"* ]]; then
        fail "Local Network granted (engine.log: denied)" "System Settings > Privacy & Security > Local Network > Nocturne on; then tools/autostart.sh restart"
    elif [[ $bonjour == *advertising* ]]; then
        pass "Local Network granted (engine advertises over Bonjour)"
    else
        manual "Local Network granted (engine.log: ${bonjour:-no bonjour line})" "System Settings > Privacy & Security > Local Network: Nocturne on (asked once on the Mini's screen at the first engine start)"
    fi
else
    manual "Local Network granted (no engine.log yet)" "start the engine once in the desktop session and answer the Local Network prompt; then rerun"
fi

if [[ ! -f $PLIST ]]; then
    fail "LaunchAgent installed" "~/Nocturne/tools/autostart.sh install -- --audio-input Focusrite [--composition ...] (or tools/show-to-mini.sh SHOW.json)"
else
    [[ $(plutil -extract ProgramArguments.0 raw "$PLIST" 2> /dev/null) == "$APP/Contents/MacOS/nocturne-supervisor" ]] &&
        pass "LaunchAgent installed, runs $APP" ||
        fail "LaunchAgent runs $APP" "~/Nocturne/tools/autostart.sh install -- ... (reinstall without --bin)"
    state=$(launchctl print "gui/$(id -u)/ai.azylume.nocturne" 2> /dev/null | awk '$1 == "state" { print $3; exit }')
    [[ $state == running ]] && pass "LaunchAgent loaded and running" ||
        fail "LaunchAgent loaded and running (state: ${state:-not loaded})" "~/Nocturne/tools/autostart.sh restart (needs the desktop session); see ~/Nocturne/tools/autostart.sh logs"
    mode=$(plutil -extract mode raw ~/Library/Application\ Support/Nocturne/State/status.json 2> /dev/null || echo unknown)
    [[ $mode == safe* ]] && fail "supervisor not in safe mode" "~/Nocturne/tools/autostart.sh logs, fix the cause, then autostart.sh restart" ||
        echo "INFO    supervisor mode: $mode"
fi
REMOTE
}

manual_steps() {
    cat <<EOF
MANUAL  reboot proof, keyboard and mouse unplugged
        do: ssh $MINI sudo shutdown -r now; the picture must come back by itself
MANUAL  a solid black desktop picture
        do: System Settings > Wallpaper > Colors > black
EOF
}

held=${1:-}
if [[ $MINI != local && $held != --locked ]]; then
    NOCTURNE_MINI=$MINI "$ROOT/tools/mini-lock.sh" acquire mini-setup-check
    trap 'NOCTURNE_MINI=$MINI "$ROOT/tools/mini-lock.sh" release' EXIT
fi

out=$(checks)
printf '%s\n' "$out"
manual_steps
printf '%s\n' "$out" | grep -q '^FAIL' && exit 1 || exit 0
