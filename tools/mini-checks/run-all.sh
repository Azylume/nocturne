#!/bin/bash
# Runs every Mini check in order and prints one table. On the Mac mini, after the deploy, with the gig show in the
# autostart and the laptop UI connected:
#   ~/Nocturne/mini-checks/run-all.sh [--dry-run] [--no-restarts] [--djlink]
# --djlink adds djlink.sh at the end: it needs tools/djlink-sim running on the laptop and sudo for tcpdump.
# About 12 minutes (5 with --no-restarts); ab-flags.sh and the audio restart blank the outputs for a few seconds each.
# Results: ~/Nocturne/mini-checks-results/DATE/ (results.tsv, stats per window, actions.log, the plist backup).
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"
args=()
djlink=0
for a in "$@"; do
    if [[ $a == --djlink ]]; then djlink=1; else args+=("$a"); fi
done
parse_args "${args[@]}"
export OUT RESULTS AB_SECONDS WARMUP SAMPLES

say "mini-checks: results in $OUT$( ((DRY)) && echo ' (dry run)')"
checks="displays perf previews record audio ab-flags ui"
((djlink)) && checks="$checks djlink"
for check in $checks; do
    "$HERE/$check.sh" "${args[@]}" || row "$check.sh" "$HERE/$check.sh" "exit $?" "exit 0" FAIL
done

echo
{
    printf 'CHECK\tCOMMAND\tVALUE\tTHRESHOLD\tRESULT\n'
    cat "$RESULTS"
} | awk -F'\t' '{ printf "%-38.38s  %-48.48s  %-60.60s  %-44.44s  %s\n", $1, $2, $3, $4, $5 }'
echo
awk -F'\t' '{ n[$5]++ } END { printf "PASS %d  FAIL %d  SKIP %d  MANUAL %d  INFO %d\n", n["PASS"], n["FAIL"], n["SKIP"], n["MANUAL"], n["INFO"] }' "$RESULTS"
echo "full rows: $RESULTS"
