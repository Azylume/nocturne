#!/bin/bash
# Runs one shard of the test suite as CI does: the engine tests of every N-th test file by name (on Windows, files
# balanced by their lavapipe seconds), or the UI tests, without the cases .github/realtime-tests.txt lists (on Windows
# also .github/windows-ci-tests.txt), as a console log and a JUnit report under build/test-reports.
# A failed case is rerun once on its own; a shard passes when the rerun does, and says which cases needed it.
# usage: tools/ci-tests.sh engine SHARD SHARDS | tools/ci-tests.sh ui
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${NOCTURNE_BUILD_DIR:-$ROOT/build}
REPORTS=$BUILD/test-reports
mkdir -p "$REPORTS"
export NOCTURNE_NO_AUDIO_INPUT=1
PYTHON=${PYTHON:-/usr/bin/python3}

skip_lists=("$ROOT/.github/realtime-tests.txt")
# On Windows a hung test binary is killed after 25 minutes, so the shard fails with its report instead of the job's
# time limit cancelling it without one.
limit=()
shard_files=every_nth_shard
case $(uname -s) in
MINGW* | MSYS*)
    skip_lists+=("$ROOT/.github/windows-ci-tests.txt")
    limit=(timeout --signal=KILL 25m)
    shard_files=lavapipe_shard
    ;;
esac
# Seconds per test file on lavapipe (a laptop at 3 threads, skip lists applied); a file not listed counts 4. The
# Windows shards take the files largest first, each to the shard with the least so far.
lavapipe_seconds="
transition_test 332 render_scale_test 303 effects_test 206 record_ffmpeg_test 130 classic_test 126
column_autopilot_test 68 autopilot_test 59 resilience_test 56 jump_reverse_test 47 atomic_write_test 40
modulation_test 37 deck_layers_test 36 composition_test 36 continuity_test 34 text_test 32 preview_test 32
trigger_test 29 clip_effects_test 29 sync_test 25 supervisor_test 23 static_bake_test 22 prototype_gaps_test 21
transport_test 20 media_test 20 screens_test 19 djlink_test 18 win_outputs_test 16 isf_engine_test 15
supervisor_process_test 13 generator_test 12 desk_test 12 protocol_test 11 mapping_test 11 layers_test 10
return_video_test 10"

every_nth_shard()
{
    ls "$ROOT"/tests/*_test.cpp "$ROOT"/tests/*_test.mm | sort | awk -v n="$1" -v m="$2" \
        'NR % m == n % m { sub(".*/", "*?tests?"); print }' | paste -sd, -
}

lavapipe_shard()
{
    ls "$ROOT"/tests/*_test.cpp | sort | table=$lavapipe_seconds awk '
        BEGIN { k = split(ENVIRON["table"], t); for (i = 1; i < k; i += 2) seconds[t[i]] = t[i + 1] }
        { file = $0; sub(".*/", "", file); base = file; sub("[.]cpp$", "", base)
          print (base in seconds ? seconds[base] : 4), file }' |
        sort -k1,1nr -k2,2 | awk -v n="$1" -v m="$2" '
        { best = 0; for (i = 1; i < m; i++) if (load[i] < load[best]) best = i
          load[best] += $1; if (best == n) print "*?tests?" $2 }' | paste -sd, -
}

skip=$(cat "${skip_lists[@]}" | grep -v '^#' | grep -v '^$' | sed 's/,/\\,/g' | paste -sd, -)

case $1 in
engine)
    shard=$2
    shards=$3
    # "?" stands for the path separator: the test binary names its files with "\" on Windows.
    files=$($shard_files "$shard" "$shards")
    name=engine-$shard
    binary=$BUILD/nocturne_tests
    filters=(--source-file="$files")
    ;;
ui)
    name=ui
    binary=$BUILD/src/ui/nocturne_ui_tests
    filters=()
    ;;
*)
    echo "usage: $0 engine SHARD SHARDS | $0 ui" >&2
    exit 2
    ;;
esac

# doctest writes every reporter to one stream, so the JUnit report goes to its file and the console summary is read
# back out of it; what the engines print on the way goes to the log.
run() {
    local status=0
    ${limit[@]+"${limit[@]}"} "$binary" ${filters[@]+"${filters[@]}"} --test-case-exclude="$skip" --reporters=junit --out="$REPORTS/$1.xml" "${@:2}" \
        > "$REPORTS/$1.log" 2>&1 || status=$?
    if [ "$status" = 137 ]; then
        echo "::error::$1 was killed after 25 minutes without a report"
        exit 1
    fi
    "$PYTHON" - "$REPORTS/$1.xml" "$1" <<'PY' || status=1
import sys, xml.etree.ElementTree as ET
# After a crash doctest writes the report twice; the last one holds every case.
text = open(sys.argv[1], encoding="utf-8").read()
tree = ET.fromstring(text[text.rindex("<testsuites"):])
cases = list(tree.iter("testcase"))
failed = [c for c in cases if c.find("failure") is not None or c.find("error") is not None]
for c in failed:
    print(f"FAILED: {c.get('name')}")
    for problem in list(c.iter("failure")) + list(c.iter("error")):
        print("  " + (problem.text or problem.get("message") or "").strip().replace("\n", "\n  "))
print(f"== {sys.argv[2]}: {len(cases) - len(failed)} passed, {len(failed)} failed")
sys.exit(1 if not cases else 0)
PY
    return $status
}

echo "== $name: $(echo "${files:-ui}" | tr ',' '\n' | wc -l | tr -d ' ') file(s), $(echo "$skip" | tr ',' '\n' | grep -c .) listed case pattern(s) skipped"
if run "$name"; then
    exit 0
fi

# The failed cases, from the JUnit report, rerun one by one. Python on Windows ends its lines with "\r\n".
failed=$("$PYTHON" -c '
import sys, xml.etree.ElementTree as ET
text = open(sys.argv[1], encoding="utf-8").read()
tree = ET.fromstring(text[text.rindex("<testsuites"):])
for case in tree.iter("testcase"):
    if case.find("failure") is not None or case.find("error") is not None:
        print(case.get("name"))
' "$REPORTS/$name.xml" | tr -d '\r')
if [ -z "$failed" ]; then
    echo "== $name: failed without a failed case in the report" >&2
    exit 1
fi
# The report names a subcase as "case/subcase" (a name may hold a slash itself), and a filter matches only whole
# cases: the rerun takes the longest "/"-cut of the name that is a test case of this shard.
cases=$("$binary" ${filters[@]+"${filters[@]}"} --list-test-cases | grep -v -e '^\[doctest\]' -e '^====' | tr -d '\r')
status=0
while IFS= read -r case; do
    name_=$case
    while [[ $name_ == */* ]] && ! grep -qxF -- "$name_" <<< "$cases"; do
        name_=${name_%/*}
    done
    echo "== $name: rerunning '$name_'"
    if run "$name-rerun" --test-case="$(printf '%s' "$name_" | sed 's/,/\\,/g')"; then
        echo "::warning::flaky: '$case' failed once and passed on its rerun ($name)"
    else
        echo "::error::'$case' failed twice ($name)"
        status=1
    fi
done <<< "$failed"
exit $status
