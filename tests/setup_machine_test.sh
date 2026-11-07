#!/bin/bash
# tools/setup-machine.sh against stub defaults, pmset, systemsetup, launchctl and sudo: a dry run changes nothing, a real
# run sets everything, a second run changes nothing, and --undo puts the first values back. Touches no real setting.
# usage: tests/setup_machine_test.sh
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'command rm -rf "$TMP"' EXIT
HOME_DIR=$TMP/home
mkdir -p "$TMP/stub" "$TMP/bin" "$HOME_DIR/Library/LaunchAgents"
printf '#!/bin/sh\n' > "$TMP/bin/nocturne"
cp "$TMP/bin/nocturne" "$TMP/bin/nocturne-supervisor"
chmod +x "$TMP/bin/nocturne" "$TMP/bin/nocturne-supervisor"
printf ' sleep 1\n displaysleep 2\n powernap 1\n' > "$TMP/pmset"
echo "Restart After Freeze: Off" > "$TMP/freeze"
printf -- '-|com.apple.dock|wvous-br-corner\t14\n' > "$TMP/defaults"
failures=0

cat > "$TMP/stub/defaults" <<'STUB'
#!/bin/bash
host=-
[[ $1 == -currentHost ]] && { host=$1; shift; }
op=$1 domain=$2 key=$3
case $op in
    read)
        v=$(awk -F'\t' -v k="$host|$domain|$key" '$1 == k { print $2 }' "$STUB_DB")
        [[ -n $v ]] || exit 1
        case $v in true) v=1 ;; false) v=0 ;; esac
        echo "$v" ;;
    write)
        grep -vF -- "$host|$domain|$key"$'\t' "$STUB_DB" > "$STUB_DB.new" || true
        printf '%s\t%s\n' "$host|$domain|$key" "$5" >> "$STUB_DB.new"
        mv "$STUB_DB.new" "$STUB_DB" ;;
    delete)
        grep -vF -- "$host|$domain|$key"$'\t' "$STUB_DB" > "$STUB_DB.new" || true
        mv "$STUB_DB.new" "$STUB_DB" ;;
esac
STUB
cat > "$TMP/stub/pmset" <<'STUB'
#!/bin/bash
if [[ $1 == -g ]]; then cat "$STUB_PM"; exit 0; fi
grep -v " $2 " "$STUB_PM" > "$STUB_PM.new" || true
printf ' %s %s\n' "$2" "$3" >> "$STUB_PM.new"
mv "$STUB_PM.new" "$STUB_PM"
STUB
cat > "$TMP/stub/systemsetup" <<'STUB'
#!/bin/bash
case $1 in
    -getrestartfreeze) cat "$STUB_FREEZE" ;;
    -setrestartfreeze) [[ $2 == on ]] && state=On || state=Off; echo "Restart After Freeze: $state" > "$STUB_FREEZE" ;;
esac
STUB
cat > "$TMP/stub/sudo" <<'STUB'
#!/bin/bash
[[ $1 == -n ]] && shift
exec "$@"
STUB
printf '#!/bin/sh\nexit 0\n' > "$TMP/stub/launchctl"
cp "$TMP/stub/launchctl" "$TMP/stub/killall"
cp "$TMP/stub/launchctl" "$TMP/stub/plutil"
chmod +x "$TMP/stub/"*

setup() {
    env HOME="$HOME_DIR" PATH="$TMP/stub:$PATH" STUB_DB="$TMP/defaults" STUB_PM="$TMP/pmset" STUB_FREEZE="$TMP/freeze" \
        "$ROOT/tools/setup-machine.sh" "$@" 2>&1
}

check() {
    local name=$1 expected=$2 actual=$3
    if [[ $actual == *"$expected"* ]]; then
        echo "ok   $name"
    else
        echo "FAIL $name: expected '$expected' in '$actual'"
        failures=$((failures + 1))
    fi
}

state() { cat "$TMP/defaults" "$TMP/pmset" "$TMP/freeze"; find "$HOME_DIR" | sort; }

before=$(state)
out=$(setup --dry-run --sudo --bin "$TMP/bin" --audio-input Focusrite -- --fps 60)
check "dry run lists the LaunchAgent with the options" "--audio-input Focusrite --fps 60" "$out"
check "dry run lists the sudo steps" "sudo pmset -a sleep 0" "$out"
check "dry run changes nothing" "$before" "$(state)"

out=$(setup --yes --sudo --bin "$TMP/bin" --audio-input Focusrite -- --fps 60)
check "apply says done" "Done." "$out"
check "plist written" "ai.azylume.nocturne" "$(ls "$HOME_DIR/Library/LaunchAgents")"
check "screen saver set" "idleTime	0" "$(cat "$TMP/defaults")"
check "pmset sleep set" " sleep 0" "$(cat "$TMP/pmset")"
check "freeze set" "On" "$(cat "$TMP/freeze")"
check "hot corner set" "wvous-br-corner	1" "$(cat "$TMP/defaults")"

applied=$(state)
out=$(setup --yes --sudo --bin "$TMP/bin" --audio-input Focusrite -- --fps 60)
check "second run has nothing to change" "Nothing to change." "$out"
check "second run changes nothing" "$applied" "$(state)"

out=$(setup --undo --yes --sudo)
check "undo says done" "Done." "$out"
check "undo restores pmset sleep" " sleep 1" "$(cat "$TMP/pmset")"
check "undo turns restart after a freeze off again" "Off" "$(cat "$TMP/freeze")"
check "undo restores the hot corner" "wvous-br-corner	14" "$(cat "$TMP/defaults")"
! grep -q idleTime "$TMP/defaults" || { echo "FAIL idleTime still set"; failures=$((failures + 1)); }
[[ ! -e $HOME_DIR/Library/Application\ Support/Nocturne/setup-machine-undo.txt ]] || { echo "FAIL undo file kept"; failures=$((failures + 1)); }

out=$(setup --bogus || true)
check "unknown option refused" "unknown option" "$out"
out=$(setup --bin "$TMP/nowhere" --dry-run || true)
check "missing bundle refused" "no folder $TMP/nowhere" "$out"

((failures == 0)) || { echo "$failures failed"; exit 1; }
echo "all passed"
