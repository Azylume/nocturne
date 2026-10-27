#!/bin/bash
# Assembles the standalone preview bundle: one Nocturne.app that holds the UI and the engine binaries, with the
# placeholder icon, then ad-hoc signs it inside-out with the hardened runtime so Apple Silicon will run it.
#
#   tools/make-bundle.sh [--ui UI_APP] [--engine ENGINE_APP] [--out OUT_APP] [--version X.Y.Z] [--build N]
#
# Defaults read the two dev bundles from ./build and write build/release/Nocturne.app. The version defaults to the
# project() version in CMakeLists.txt. This makes NO Developer ID signature: the result is unsigned to Gatekeeper
# (spctl rejects it, on purpose). A signed, notarised build is #32.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
UI_APP="$here/build/Nocturne UI.app"
ENGINE_APP="$here/build/Nocturne.app"
OUT_APP="$here/build/release/Nocturne.app"
ENTITLEMENTS="$here/tools/nocturne.entitlements"
ICON="$here/assets/icon/nocturne.icns"
VERSION=
BUILD=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --ui) UI_APP=$2; shift 2 ;;
        --engine) ENGINE_APP=$2; shift 2 ;;
        --out) OUT_APP=$2; shift 2 ;;
        --version) VERSION=$2; shift 2 ;;
        --build) BUILD=$2; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

if [[ -z $VERSION ]]; then
    VERSION=$(sed -n 's/^project(nocturne VERSION \([0-9.]*\).*/\1/p' "$here/CMakeLists.txt" | head -n 1)
fi
[[ -n $VERSION ]] || { echo "no version; pass --version" >&2; exit 1; }

for app in "$UI_APP" "$ENGINE_APP"; do
    [[ -d $app ]] || { echo "no bundle at $app (build it first)" >&2; exit 1; }
done
[[ -f $ICON ]] || { echo "no icon at $ICON (run assets/icon/make-icns.sh)" >&2; exit 1; }

echo "assembling $OUT_APP (version $VERSION, build $BUILD)"
rm -rf "$OUT_APP"
mkdir -p "$(dirname "$OUT_APP")"
cp -R "$UI_APP" "$OUT_APP"

macos="$OUT_APP/Contents/MacOS"
resources="$OUT_APP/Contents/Resources"
mkdir -p "$macos" "$resources"

# The engine, its supervisor and the two tools move in beside the UI binary, so EngineLauncher finds them by the
# bundle path.
for exe in nocturne nocturne-supervisor nocturne-ctl nocturne-desk; do
    cp "$ENGINE_APP/Contents/MacOS/$exe" "$macos/$exe"
done

cp "$ICON" "$resources/nocturne.icns"

plist="$OUT_APP/Contents/Info.plist"
set_plist() { plutil -replace "$1" -"$2" "$3" "$plist"; }
set_plist CFBundleIdentifier string "com.azylume.nocturne"
set_plist CFBundleName string "Nocturne"
set_plist CFBundleDisplayName string "Nocturne"
set_plist CFBundleIconFile string "nocturne"
set_plist CFBundleShortVersionString string "$VERSION"
set_plist CFBundleVersion string "$BUILD"
set_plist LSMinimumSystemVersion string "14.0"
# The engine the UI spawns captures audio; its prompts attribute to this bundle.
set_plist NSAudioCaptureUsageDescription string \
    "Nocturne analyses the system's or an app's audio so visuals can react to the music."
set_plist NSMicrophoneUsageDescription string \
    "Nocturne analyses the audio input so visuals can react to the music."
plutil -lint "$plist" > /dev/null

# Ad-hoc sign inside-out: every nested binary first, then the bundle. Apple Silicon refuses to run unsigned arm64 code,
# so even this placeholder signature matters. --timestamp=none keeps it offline.
sign() { codesign --force --options runtime --timestamp=none --entitlements "$ENTITLEMENTS" -s - "$1"; }
for exe in nocturne nocturne-supervisor nocturne-ctl nocturne-desk "Nocturne UI"; do
    echo "signing Contents/MacOS/$exe"
    sign "$macos/$exe"
done
echo "signing the bundle"
sign "$OUT_APP"

echo "verifying"
codesign --verify --deep --strict --verbose=2 "$OUT_APP"
echo "--- spctl (expected to reject: this build is unsigned to Gatekeeper) ---"
spctl -a -vv "$OUT_APP" || true
echo "--- done: $OUT_APP"
