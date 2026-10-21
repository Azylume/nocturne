#!/bin/bash
# Which Mini the tools would reach, from MINI, NOCTURNE_MINI, the config file or the default. Connects to nothing:
# ssh is a stub that prints its arguments.
# usage: tests/mini_host_test.sh
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'command rm -rf "$TMP"' EXIT
mkdir -p "$TMP/home/.config/nocturne" "$TMP/bin"
printf '#!/bin/sh\necho "ssh $*"\n' > "$TMP/bin/ssh"
chmod +x "$TMP/bin/ssh"
CONFIG=$TMP/home/.config/nocturne/mini
failures=0

check() {
    local name=$1 expected=$2 actual=$3
    if [[ $actual == *"$expected"* ]]; then
        echo "ok   $name"
    else
        echo "FAIL $name: expected '$expected' in '$actual'"
        failures=$((failures + 1))
    fi
}

# Every run starts clean: no MINI or NOCTURNE_MINI from the caller, HOME without the real config.
tool() {
    env -u MINI -u NOCTURNE_MINI -u NOCTURNE_MINI_CONFIG HOME="$TMP/home" PATH="$TMP/bin:$PATH" "$@" 2>&1 || true
}

check "default" "mini.local (default)" "$(tool "$ROOT/tools/mini-host.sh")"
printf '  mini.tail1234.ts.net \n' > "$CONFIG"
check "config file" "mini.tail1234.ts.net ($CONFIG)" "$(tool "$ROOT/tools/mini-host.sh")"
check "NOCTURNE_MINI over the file" "mini (NOCTURNE_MINI)" \
    "$(tool env NOCTURNE_MINI=mini "$ROOT/tools/mini-host.sh")"
check "MINI over NOCTURNE_MINI" "admin@100.101.102.103 (MINI)" \
    "$(tool env NOCTURNE_MINI=mini MINI=admin@100.101.102.103 "$ROOT/tools/mini-host.sh")"
check "IPv6 address" "fd7a:115c:a1e0::1 (MINI)" "$(tool env MINI=fd7a:115c:a1e0::1 "$ROOT/tools/mini-host.sh")"
check "MINI=local stays" "local (MINI)" "$(tool env MINI=local "$ROOT/tools/mini-host.sh")"
check "MINI_HOST is the host alone" "mini.tail1234.ts.net" \
    "$(tool bash -c ". '$ROOT/tools/mini-host.sh'; echo \$MINI_HOST")"
printf '\n' > "$CONFIG"
check "an empty config file falls back" "mini.local (default)" "$(tool "$ROOT/tools/mini-host.sh")"

printf 'mini\n' > "$CONFIG"
check "mini-lock.sh uses the config" "ssh mini cat" "$(tool "$ROOT/tools/mini-lock.sh" status)"
check "mini-lock.sh uses NOCTURNE_MINI" "ssh other cat" \
    "$(tool env NOCTURNE_MINI=other "$ROOT/tools/mini-lock.sh" status)"
dry=$(tool "$ROOT/tools/show-to-mini.sh" --dry-run --no-build show.json)
check "show-to-mini.sh ssh" "+ ssh mini " "$dry"
check "show-to-mini.sh ctl" "--host mini --port 7400" "$dry"
check "show-to-mini.sh lock" "NOCTURNE_MINI=mini" "$dry"

for f in "$ROOT"/tools/*.sh; do
    [[ $f == */mini-host.sh ]] && continue
    if grep -q '192\.168\.0\.131' "$f"; then
        echo "FAIL $(basename "$f") has an address of its own"
        failures=$((failures + 1))
    fi
    if grep -q '\$MINI\b\|"\$MINI"\|HOST=\$MINI' "$f" && ! grep -q 'mini-host\.sh"' "$f"; then
        echo "FAIL $(basename "$f") uses MINI without tools/mini-host.sh"
        failures=$((failures + 1))
    fi
done

((failures == 0)) && echo "mini_host_test: PASS" || { echo "mini_host_test: $failures failed"; exit 1; }
