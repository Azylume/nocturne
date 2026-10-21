#!/bin/bash
# usage: sign-bundle.sh APP | sign-bundle.sh --ui UI_APP
# Signs Nocturne.app with the "Nocturne Dev" identity (tools/make-signing-identity.sh), helpers first, then the bundle.
# macOS keys privacy permissions on the bundle identifier plus that certificate, so they survive rebuilds. Without the
# identity it signs ad hoc, and every build then needs the permissions granted again.
# With --ui it signs the JUCE UI's own bundle, "Nocturne UI.app" (com.azylume.nocturne.ui), with the same identity.
set -euo pipefail

UI=0
if [ "$1" = "--ui" ]; then
    UI=1
    shift
fi
APP=$1
ID=com.azylume.nocturne
IDENTITY="Nocturne Dev"

if security find-identity -p codesigning | grep -q "\"$IDENTITY\""; then
    sign=(codesign --force --timestamp=none --sign "$IDENTITY")
else
    echo "warning: ==================================================================" >&2
    echo "warning: no \"$IDENTITY\" signing identity; signing AD HOC. macOS will ask for the" >&2
    echo "warning: microphone and local network again after every build. Run tools/make-signing-identity.sh." >&2
    echo "warning: ==================================================================" >&2
    sign=(codesign --force --sign -)
fi

if [ $UI = 1 ]; then
    "${sign[@]}" --identifier "$ID.ui" "$APP"
    exit 0
fi

"${sign[@]}" --identifier "$ID.supervisor" "$APP/Contents/MacOS/nocturne-supervisor"
"${sign[@]}" --identifier "$ID.ctl" "$APP/Contents/MacOS/nocturne-ctl"
"${sign[@]}" --identifier "$ID.desk" "$APP/Contents/MacOS/nocturne-desk"
"${sign[@]}" --identifier "$ID" "$APP"
