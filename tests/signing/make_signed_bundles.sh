#!/bin/bash
# Regenerates the signed fake bundles the Update signer tests check, in this folder:
#   running.app   the "installed" bundle, signed by identity A
#   same.app      another build signed by A
#   other.app     signed by identity B, a different certificate with the same name and bundle identifier
#   adhoc.app     signed ad hoc
#   unsigned.app  not signed
# Both identities are throwaway self-signed certificates in a temporary keychain, deleted afterwards. codesign only
# finds an identity in a keychain on the user's search list, so the temporary keychain joins it while signing and the
# list is put back on exit.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ID=com.azylume.nocturne
work=$(mktemp -d)
keychain="$work/signing.keychain"
saved=()
while IFS= read -r line; do
    saved+=("$(sed 's/^ *"//; s/"$//' <<< "$line")")
done < <(security list-keychains -d user)
restore() {
    security list-keychains -d user -s "${saved[@]}"
    security delete-keychain "$keychain" 2> /dev/null || true
    command rm -rf "$work"
}
trap restore EXIT

security create-keychain -p signing "$keychain"
security set-keychain-settings "$keychain"
security unlock-keychain -p signing "$keychain"
for who in A B; do
    openssl req -x509 -newkey rsa:2048 -nodes -days 36500 -subj "/CN=Nocturne Dev" \
        -keyout "$work/$who.key" -out "$work/$who.pem" \
        -addext "basicConstraints=critical,CA:false" \
        -addext "keyUsage=critical,digitalSignature" \
        -addext "extendedKeyUsage=critical,codeSigning" 2> /dev/null
    openssl pkcs12 -export -legacy -inkey "$work/$who.key" -in "$work/$who.pem" -name "Nocturne Dev $who" \
        -passout pass:p -out "$work/$who.p12"
    security import "$work/$who.p12" -k "$keychain" -P p -T /usr/bin/codesign > /dev/null
done
security set-key-partition-list -S apple-tool:,apple: -s -k signing "$keychain" > /dev/null
security list-keychains -d user -s "${saved[@]}" "$keychain"

hash() {
    openssl x509 -in "$work/$1.pem" -outform der | shasum -a 1 | cut -d' ' -f1
}

bundle() {
    local app="$HERE/$1.app"
    command rm -rf "$app"
    mkdir -p "$app/Contents/MacOS"
    printf '#!/bin/bash\n# nocturne-build %s\n' "$2" > "$app/Contents/MacOS/nocturne"
    chmod +x "$app/Contents/MacOS/nocturne"
    cat > "$app/Contents/Info.plist" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>nocturne</string>
    <key>CFBundleIdentifier</key>
    <string>$ID</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
</dict>
</plist>
EOF
}

sign() {
    codesign --force --timestamp=none --keychain "$keychain" --sign "$(hash "$1")" --identifier "$ID" "$HERE/$2.app"
}

bundle running 20261017000000-aaaa
sign A running
bundle same 20261018000000-bbbb
sign A same
bundle other 20261018000000-bbbb
sign B other
bundle adhoc 20261018000000-bbbb
codesign --force --sign - --identifier "$ID" "$HERE/adhoc.app"
bundle unsigned 20261018000000-bbbb
for app in running same other adhoc; do
    codesign -d -r- "$HERE/$app.app" 2>&1 | grep designated
done
