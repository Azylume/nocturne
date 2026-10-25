#!/bin/bash
# Runs the cases .github/realtime-tests.txt keeps out of CI, each RUNS times on its own, to sort them: those that
# pass every time can come off the list, those that fail every time need what the runner lacks, and the rest are
# timing-sensitive. Prints one Markdown row per entry: passes, runs, seconds per run, the binary and, for a failure,
# its first assertion. Entries are split into PARTS by line, this run taking PART.
# usage: tools/realtime-triage.sh [RUNS [PART PARTS]]   (defaults: 10, 0 1)
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${NOCTURNE_BUILD_DIR:-$ROOT/build}
RUNS=${1:-10}
PART=${2:-0}
PARTS=${3:-1}
# One run of a case may take this long before it counts as failed.
LIMIT=${TRIAGE_LIMIT:-300}
OUT=$BUILD/triage
mkdir -p "$OUT"
export NOCTURNE_NO_AUDIO_INPUT=1

engine=$BUILD/nocturne_tests
ui=$BUILD/src/ui/nocturne_ui_tests

count() {
    "$1" --list-test-cases --test-case="$2" 2>/dev/null | sed -n 's/.*passing the current filters: \([0-9]*\).*/\1/p'
}

echo "| entry | passed | s/run | binary | first failure |"
echo "|---|---|---|---|---|"
index=0
grep -v '^#' "$ROOT/.github/realtime-tests.txt" | grep -v '^$' | while IFS= read -r entry; do
    index=$((index + 1))
    (( index % PARTS == PART % PARTS )) || continue
    pattern=$(printf '%s' "$entry" | sed 's/,/\\,/g')
    binary=
    for candidate in "$engine" "$ui"; do
        if [ "$(count "$candidate" "$pattern")" != 0 ]; then
            binary=$candidate
            break
        fi
    done
    if [ -z "$binary" ]; then
        echo "| \`$entry\` | - | - | none | matches no case |"
        continue
    fi
    passed=0
    first=
    start=$(date +%s)
    for run in $(seq "$RUNS"); do
        log=$OUT/$index-$run.log
        if perl -e 'alarm shift; exec @ARGV' "$LIMIT" "$binary" --test-case="$pattern" > "$log" 2>&1; then
            passed=$((passed + 1))
        elif [ -z "$first" ]; then
            first=$(grep -m1 -E 'ERROR|FATAL|timed out' "$log" | sed 's/.*tests\///; s/|/\\|/g' | cut -c1-140)
            [ -n "$first" ] || first="exit $(tail -1 "$log" | cut -c1-80)"
        fi
    done
    seconds=$(( ($(date +%s) - start) / RUNS ))
    echo "| \`$entry\` | $passed/$RUNS | $seconds | $(basename "$binary") | $first |"
done
