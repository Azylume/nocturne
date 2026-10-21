#!/bin/bash
# usage: stress-tests.sh [runs] [build dir] [name filter]
# Runs each test that once failed under load (ticket 100) `runs` times in a row (default 20) under tools/load.sh, at
# nice 15 and under the build lock, and reports every failure with its log. The optional filter (grep -i) picks
# tests by name. A run that printed "UNDECIDED:" (load kept a test from telling, so it retried) counts apart; more than
# AZL_STRESS_UNDECIDED (default one in ten runs) fails the test too, so a regression cannot hide behind load. Exits 1
# when any test failed.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
runs="${1:-20}"
build="$(cd "${2:-$here/../build}" && pwd)"
filter="${3:-}"
undecidedMax="${AZL_STRESS_UNDECIDED:-$((runs / 10))}"
if [ -z "${AZL_STRESS_LOCKED:-}" ]; then
  AZL_STRESS_LOCKED=1 AZL_STRESS_UNDECIDED="$undecidedMax" exec lockf /private/tmp/azl-build.lock "$0" "$runs" "$build" "$filter"
fi

# binary|doctest filter; commas in names are '?', since doctest splits filters on commas.
TESTS=(
  "nocturne_tests|-tc=a subscribed client gets state messages at its rate until it unsubscribes or leaves"
  "nocturne_tests|-tc=a full disk stops the recording cleanly with one notice? and the show goes on"
  "nocturne_tests|-tc=a resumed show is held black until its clip is decoded mid-way? then shows where the beat clock has got to"
  "nocturne_tests|-tc=Autopilot resumes on the clip it had stepped to"
  "nocturne_tests|-tc=clip preview memory stays bounded across many selections? effect edits and on/off toggles"
  "nocturne_tests|-tc=on a slow decoder? resyncs on a Random Jump clip miss no frames"
  "nocturne_tests|-tc=the selected clip's preview never costs the show*"
  "nocturne_tests|-tc=preloads open and unwanted decoders close spread out*"
  "nocturne_tests|-tc=a main engine restarted without its show does not wipe the node's show"
  "nocturne_tests|-tc=a node without fetching? as a UI's preview follower? mirrors the composition but copies no media"
  "nocturne_tests|-tc=once locked? every output shows each rendered frame? even when display callbacks run late"
  "src/ui/nocturne_ui_tests|-tc=steps: an effect card's Autopilot badge cycles*"
  "src/ui/nocturne_ui_tests|-sf=*map_test.cpp"
  "src/ui/nocturne_ui_tests|-sf=*monitor_handles_test.cpp"
  "src/ui/nocturne_ui_tests|-sf=*panels_test.cpp"
  "src/ui/nocturne_ui_tests|-sf=*parity_test.cpp"
)

logs="${TMPDIR:-/tmp}/azl-stress-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$logs"
"$here/load.sh" start
trap '"$here/load.sh" stop' EXIT INT TERM

failed=0
report=()
for entry in "${TESTS[@]}"; do
  binary="${entry%%|*}"
  pattern="${entry#*|}"
  [ -n "$filter" ] && ! grep -qi -- "$filter" <<< "$pattern" && continue
  name="${pattern#-??=}"
  fails=0
  undecided=0
  started=$SECONDS
  for i in $(seq "$runs"); do
    log="$logs/$(tr -c 'A-Za-z0-9\n' '_' <<< "${name:0:60}")-$i.log"
    if (cd "$(dirname "$build/$binary")" && nice -n 15 "./$(basename "$binary")" "$pattern") > "$log" 2>&1 &&
      ! grep -q "test cases: *0 |" "$log"; then
      if grep -q "UNDECIDED:" "$log"; then
        undecided=$((undecided + 1))
        echo "UNDECIDED $name (run $i): $log"
      else
        command rm -f "$log"
      fi
    else
      fails=$((fails + 1))
      echo "FAIL $name (run $i): $log"
    fi
  done
  report+=("$fails/$runs failed, $undecided undecided  $((SECONDS - started))s  $name")
  [ "$fails" -eq 0 ] && [ "$undecided" -le "$undecidedMax" ] || failed=1
done

echo
printf '%s\n' "${report[@]}"
[ "$failed" -eq 0 ] && command rm -rf "$logs"
exit "$failed"
