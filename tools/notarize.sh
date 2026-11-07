#!/bin/bash
# Submits a signed Nocturne.app or .dmg to Apple's notary service, waits for the verdict and staples the ticket.
# CI only (#32). Reads an App Store Connect API key from the environment; nothing is written outside a temp folder.
#
#   APPLE_API_KEY_BASE64=... APPLE_API_KEY_ID=... APPLE_API_ISSUER=... tools/notarize.sh PATH
#
# APPLE_API_KEY_BASE64 is the base64 of the AuthKey_XXXX.p8 file. Notarise the app first and the DMG after it.
set -euo pipefail

target=${1:?usage: notarize.sh APP_OR_DMG}
: "${APPLE_API_KEY_BASE64:?APPLE_API_KEY_BASE64 is not set}"
: "${APPLE_API_KEY_ID:?APPLE_API_KEY_ID is not set}"
: "${APPLE_API_ISSUER:?APPLE_API_ISSUER is not set}"

umask 077
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
echo "$APPLE_API_KEY_BASE64" | base64 --decode > "$work/key.p8"
auth=(--key "$work/key.p8" --key-id "$APPLE_API_KEY_ID" --issuer "$APPLE_API_ISSUER")

upload=$target
if [[ -d $target ]]; then
    upload="$work/$(basename "$target").zip"
    ditto -c -k --keepParent "$target" "$upload"
fi

echo "submitting $upload"
xcrun notarytool submit "$upload" "${auth[@]}" --wait --output-format json > "$work/result.json" || true
cat "$work/result.json"

status=$(plutil -extract status raw -o - "$work/result.json" 2> /dev/null || true)
if [[ $status != Accepted ]]; then
    echo "notarisation status: ${status:-unknown}" >&2
    id=$(plutil -extract id raw -o - "$work/result.json" 2> /dev/null || true)
    [[ -z $id ]] || xcrun notarytool log "$id" "${auth[@]}" >&2 || true
    exit 1
fi

xcrun stapler staple "$target"
xcrun stapler validate "$target"
