#!/bin/bash
# Imports the Developer ID Application certificate into a throwaway keychain and prints the identity name on stdout.
# CI only (the release workflow, #32); a developer machine uses tools/make-signing-identity.sh.
#
#   APPLE_CERT_P12_BASE64=... APPLE_CERT_PASSWORD=... tools/import-signing-cert.sh KEYCHAIN_PATH
#
# Remove the keychain afterwards: security delete-keychain KEYCHAIN_PATH
set -euo pipefail

keychain=${1:?usage: import-signing-cert.sh KEYCHAIN_PATH}
: "${APPLE_CERT_P12_BASE64:?APPLE_CERT_P12_BASE64 is not set}"
: "${APPLE_CERT_PASSWORD:?APPLE_CERT_PASSWORD is not set}"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
password=$(uuidgen)

echo "$APPLE_CERT_P12_BASE64" | base64 --decode > "$work/cert.p12"
security create-keychain -p "$password" "$keychain" >&2
security set-keychain-settings -lut 21600 "$keychain" >&2
security unlock-keychain -p "$password" "$keychain" >&2
security import "$work/cert.p12" -k "$keychain" -P "$APPLE_CERT_PASSWORD" -T /usr/bin/codesign >&2
security set-key-partition-list -S apple-tool:,apple: -s -k "$password" "$keychain" > /dev/null

existing=$(security list-keychains -d user | tr -d '"')
# shellcheck disable=SC2086 # the list is paths without spaces on a CI runner
security list-keychains -d user -s "$keychain" $existing >&2

identity=$(security find-identity -v -p codesigning "$keychain" | sed -n 's/.*"\(Developer ID Application:.*\)"/\1/p' | head -n 1)
[[ -n $identity ]] || { echo "no \"Developer ID Application\" identity in the imported certificate" >&2; exit 1; }
echo "$identity"
