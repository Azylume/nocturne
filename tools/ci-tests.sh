#!/bin/bash
# Runs one shard of the test suite as CI does: the engine tests of every N-th test file (by name), or the UI tests,
# without the cases .github/realtime-tests.txt lists (on Windows also .github/windows-ci-tests.txt), as a console log
# and a JUnit report under build/test-reports.
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
case $(uname -s) in
MINGW* | MSYS*) skip_lists+=("$ROOT/.github/windows-ci-tests.txt") ;;
esac
skip=$(cat "${skip_lists[@]}" | grep -v '^#' | grep -v '^$' | sed 's/,/\\,/g' | paste -sd, -)

case $1 in
engine)
    shard=$2
    shards=$3
    # "?" stands for the path separator: the test binary names its files with "\" on Windows.
    files=$(ls "$ROOT"/tests/*_test.cpp "$ROOT"/tests/*_test.mm | sort | awk -v n="$shard" -v m="$shards" \
        'NR % m == n % m { sub(".*/", "*?tests?"); print }' | paste -sd, -)
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
    "$binary" ${filters[@]+"${filters[@]}"} --test-case-exclude="$skip" --reporters=junit --out="$REPORTS/$1.xml" "${@:2}" \
        > "$REPORTS/$1.log" 2>&1 || status=$?
    "$PYTHON" - "$REPORTS/$1.xml" "$1" <<'PY' || status=1
import sys, xml.etree.ElementTree as ET
tree = ET.parse(sys.argv[1])
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

echo "== $name: $(echo "${files:-ui}" | tr ',' '\n' | wc -l | tr -d ' ') file(s)"
if run "$name"; then
    exit 0
fi

# The failed cases, from the JUnit report, rerun one by one.
failed=$("$PYTHON" -c '
import sys, xml.etree.ElementTree as ET
tree = ET.parse(sys.argv[1])
for case in tree.iter("testcase"):
    if case.find("failure") is not None or case.find("error") is not None:
        print(case.get("name"))
' "$REPORTS/$name.xml")
if [ -z "$failed" ]; then
    echo "== $name: failed without a failed case in the report" >&2
    exit 1
fi
# The report names a subcase as "case/subcase" (a name may hold a slash itself): the whole case reruns, found by
# taking "/..." off the end until the filter runs something.
status=0
while IFS= read -r case; do
    name_=$case
    while :; do
        echo "== $name: rerunning '$name_'"
        if run "$name-rerun" --test-case="$(printf '%s' "$name_" | sed 's/,/\\,/g')"; then
            echo "::warning::flaky: '$case' failed once and passed on its rerun ($name)"
            break
        fi
        if grep -q '<testcase' "$REPORTS/$name-rerun.xml" || [[ $name_ != */* ]]; then
            echo "::error::'$case' failed twice ($name)"
            status=1
            break
        fi
        name_=${name_%/*}
    done
done <<< "$failed"
exit $status
