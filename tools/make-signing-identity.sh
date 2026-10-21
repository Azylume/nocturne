#!/bin/bash
# Creates the self-signed code-signing identity "Nocturne Dev" in the login keychain, once; does nothing if it exists.
# Every build is signed with it, so macOS privacy permissions (microphone, local network) survive rebuilds.
# Keep it: deleting and recreating it changes the certificate, and every machine must grant the permissions again.
set -euo pipefail

NAME="Nocturne Dev"
KEYCHAIN="$HOME/Library/Keychains/login.keychain-db"

if security find-identity -p codesigning "$KEYCHAIN" | grep -q "\"$NAME\""; then
    echo "$NAME: already in the login keychain"
    exit 0
fi

work=$(mktemp -d)
trap 'command rm -rf "$work"' EXIT
pass=$(openssl rand -hex 16)

openssl req -x509 -newkey rsa:2048 -nodes -days 7300 -subj "/CN=$NAME" \
    -keyout "$work/key.pem" -out "$work/cert.pem" \
    -addext "basicConstraints=critical,CA:false" \
    -addext "keyUsage=critical,digitalSignature" \
    -addext "extendedKeyUsage=critical,codeSigning" 2> /dev/null
openssl pkcs12 -export -legacy -inkey "$work/key.pem" -in "$work/cert.pem" -name "$NAME" \
    -passout "pass:$pass" -out "$work/identity.p12"
security import "$work/identity.p12" -k "$KEYCHAIN" -P "$pass" -T /usr/bin/codesign
echo "$NAME: created in the login keychain"
