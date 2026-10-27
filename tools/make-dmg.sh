#!/bin/bash
# Packs the assembled Nocturne.app (tools/make-bundle.sh) into Nocturne-<version>-unsigned.dmg with an Applications
# symlink and a short FIRST LAUNCH.txt, and writes a SHA-256 checksum beside it. Uses hdiutil, so it needs no extra
# tool. This DMG is unsigned to Gatekeeper on purpose; a signed, notarised one is #32.
#
#   tools/make-dmg.sh [--app NOCTURNE_APP] [--version X.Y.Z] [--out DIR]
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
APP="$here/build/release/Nocturne.app"
OUT="$here/build/release"
VERSION=

while [[ $# -gt 0 ]]; do
    case "$1" in
        --app) APP=$2; shift 2 ;;
        --version) VERSION=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

[[ -d $APP ]] || { echo "no bundle at $APP (run tools/make-bundle.sh first)" >&2; exit 1; }
if [[ -z $VERSION ]]; then
    VERSION=$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$APP/Contents/Info.plist" 2> /dev/null || true)
fi
[[ -n $VERSION ]] || { echo "no version; pass --version" >&2; exit 1; }

mkdir -p "$OUT"
dmg="$OUT/Nocturne-$VERSION-unsigned.dmg"
stage=$(mktemp -d)

cp -R "$APP" "$stage/Nocturne.app"
ln -s /Applications "$stage/Applications"

cat > "$stage/FIRST LAUNCH.txt" <<EOF
Nocturne $VERSION - unsigned preview
====================================

This is an early preview with NO Apple Developer ID signature yet, so macOS
cannot verify the developer. It is safe, just not notarised. A signed, notarised
build comes later.

To install: drag Nocturne.app onto the Applications folder in this window.

To open the first time, macOS will refuse a plain double-click:

  macOS 15 (Sequoia) and newer
    1. Double-click Nocturne. You will see "Apple could not verify ...".
    2. Click Done, open System Settings > Privacy & Security, scroll to Security.
    3. Click "Open Anyway" next to Nocturne, then confirm.

  macOS 14 (Sonoma)
    Right-click (or Control-click) Nocturne and choose Open, then Open again.

Terminal alternative (removes the download quarantine flag):
    xattr -dr com.apple.quarantine /Applications/Nocturne.app

Verify the download first with the checksum published on the release:
    shasum -a 256 Nocturne-$VERSION-unsigned.dmg

Opening Nocturne starts its engine in the background and plays a show; quitting
Nocturne stops it. Audio and local-network prompts on first launch are expected.
EOF

echo "building $dmg"
rm -f "$dmg"
hdiutil create -volname "Nocturne $VERSION" -srcfolder "$stage" -ov -format UDZO "$dmg" > /dev/null
rm -rf "$stage"

( cd "$OUT" && shasum -a 256 "$(basename "$dmg")" > "$(basename "$dmg").sha256" )

echo "--- done"
echo "dmg:      $dmg"
echo "size:     $(du -h "$dmg" | cut -f1)"
echo "checksum: $(cat "$dmg.sha256")"
