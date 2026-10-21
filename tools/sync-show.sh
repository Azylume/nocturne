#!/bin/bash
# Copies a composition file and the media it references to the Mac mini:
#   ~/Nocturne/shows/FILE        the composition file
#   ~/Nocturne/media/...         its media, at their paths relative to the media root they were saved with
# Media outside that root go to ~/Nocturne/media/ by file name; the engine finds them by name. Only what changed is
# copied (rsync over ssh), nothing on the Mini is deleted, and the engine bundle is not touched. Holds the Mini lock.
# On the Mini, play it with:
#   ~/Nocturne/tools/autostart.sh install -- --composition ~/Nocturne/shows/FILE --media-root ~/Nocturne/media
# or load it into the running engine: nocturne-ctl load ~/Nocturne/shows/FILE ~/Nocturne/media
#
# usage: tools/sync-show.sh [--locked] FILE [MEDIA_ROOT]     MEDIA_ROOT: where the media are on this machine (default: as
#   saved); --locked: the caller holds the Mini lock
#   MINI=user@host or a host name (default: tools/mini-host.sh), CTL=path to nocturne-ctl (default: the build's)
set -euo pipefail

held=
[[ ${1:-} == --locked ]] && held=1 && shift
FILE=${1:?usage: tools/sync-show.sh [--locked] FILE [MEDIA_ROOT]}
. "$(dirname "$0")/mini-host.sh"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SSH="ssh -o BatchMode=yes -o ConnectTimeout=10"

if [[ -z ${CTL:-} ]]; then
    for c in "$ROOT/build/nocturne-ctl" "$ROOT/build/Nocturne.app/Contents/MacOS/nocturne-ctl"; do
        [[ -x $c ]] && CTL=$c && break
    done
fi
[[ -x ${CTL:-} ]] || { echo "sync-show: no nocturne-ctl; build first or set CTL" >&2; exit 1; }

staging=$(mktemp -d)
locked=
cleanup() {
    command rm -rf "$staging"
    [[ -z $locked ]] || NOCTURNE_MINI=$MINI "$ROOT/tools/mini-lock.sh" release
}
trap cleanup EXIT

missing=0
while IFS=$'\t' read -r source destination; do
    if [[ ! -f $source ]]; then
        echo "sync-show: missing $source" >&2
        missing=$((missing + 1))
        continue
    fi
    mkdir -p "$staging/media/$(dirname "$destination")"
    ln -s "$source" "$staging/media/$destination"
done < <("$CTL" showmedia "$FILE" ${2:+"$2"})
mkdir -p "$staging/shows"
cp "$FILE" "$staging/shows/"

if [[ -z $held ]]; then
    NOCTURNE_MINI=$MINI "$ROOT/tools/mini-lock.sh" acquire sync-show
    locked=1
fi
$SSH "$MINI" 'mkdir -p ~/Nocturne/shows ~/Nocturne/media'
rsync -rtL --partial --progress -e "$SSH" "$staging/media/" "$MINI:Nocturne/media/"
rsync -rt -e "$SSH" "$staging/shows/" "$MINI:Nocturne/shows/"
echo "synced $(basename "$FILE") to $MINI:~/Nocturne/shows ($missing media missing here)"
