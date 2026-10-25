#!/bin/bash
# Checks every Mach-O binary in the given app bundles holds both the x86_64 and the arm64 slice.
# usage: tools/check-universal.sh APP...
set -euo pipefail

status=0
for app in "$@"; do
    while IFS= read -r -d '' file; do
        if ! file -b "$file" | grep -q "Mach-O"; then
            continue
        fi
        archs=$(lipo -archs "$file")
        if [[ $archs == *x86_64* && $archs == *arm64* ]]; then
            echo "ok   $file: $archs"
        else
            echo "FAIL $file: $archs"
            status=1
        fi
    done < <(find "$app" -type f -perm -u+x -print0)
done
exit $status
