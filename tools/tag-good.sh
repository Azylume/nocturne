#!/bin/bash
# Tags HEAD good/<date>-<what>, only when the Mac mini benchmark passes for its code. Refuses on a dirty tree. A
# committed result of a commit whose code equals HEAD's (only bench/results differ) is judged again; otherwise
# tools/bench-mini.sh runs, and on PASS its result is committed and that commit is tagged.
#
# usage: tools/tag-good.sh WHAT
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

refuse() {
    echo "tag-good: refusing: $*" >&2
    exit 1
}

field() {
    python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])' "$1" "$2"
}

# The newest clean result whose commit has the same code as HEAD.
fresh_result() {
    local file commit
    for file in $(ls -r bench/results/*.json 2> /dev/null); do
        [[ $(field "$file" dirty) == False ]] || continue
        commit=$(field "$file" commit)
        if git cat-file -e "$commit^{commit}" 2> /dev/null &&
            git diff --quiet "$commit" HEAD -- . ':(exclude)bench/results'; then
            echo "$file"
            return
        fi
    done
    return 1
}

[[ $# -eq 1 && -n $1 ]] || { echo "usage: tools/tag-good.sh WHAT" >&2; exit 2; }
tag="good/$(date +%Y-%m-%d)-$1"
[[ -z $(git status --porcelain) ]] || refuse "the working tree is dirty"
! git rev-parse -q --verify "refs/tags/$tag" > /dev/null || refuse "$tag exists"

if result=$(fresh_result); then
    echo "using $result"
    tools/bench-mini.sh --verdict "$result" || refuse "the benchmark FAILS"
else
    tools/bench-mini.sh || refuse "the benchmark FAILS (result left uncommitted in bench/results)"
    result=$(ls bench/results/*.json | sort | tail -1)
    git add "$result"
    git commit -q -m "Benchmark result for $(field "$result" commit): PASS"
fi
git tag "$tag"
echo "tagged $tag at $(git rev-parse --short HEAD)"
