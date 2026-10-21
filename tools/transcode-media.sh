#!/bin/bash
# Prepares the media a Resolume composition (.avc) references for Nocturne, once, into one folder:
#   - DXV and other codecs AVFoundation cannot decode, and anything larger than 1080p, are transcoded to H.264 1080p
#     at their own frame rate (STEM.mp4);
#   - still images become a 4 s H.264 loop over black (STEM.mp4); H.264 has no alpha, so transparency turns black;
#   - H.264, HEVC, ProRes and HAP files up to 1080p are copied as they are: an APFS clone where it can be, which
#     takes no space (not a symlink: the engine reads a symlink's own size and takes the file for truncated).
# H.264 gets a keyframe every second (-g = the frame rate, no scene-cut keyframes): a loop restarts on a keyframe, a
# Random Jump or a resync decodes at most one second from its keyframe, and a reversing layer's cache of two GOPs
# stays near 0.5 GB at 1080p. On these 8-28 s loops that costs about a tenth more bits than a long GOP.
# Existing outputs are kept, so a rerun only does what is missing. Originals are never touched.
#
# With HAP set (hap, hap-alpha, hap-q or hap-q-alpha), every video and still that is not HAP already becomes a HAP movie
# up to 1080p instead (STEM.mov): ffmpeg decodes it to raw RGBA and nocturne-hap-encode (our encoder; Homebrew's ffmpeg
# has no HAP encoder) compresses it. Every HAP frame is a keyframe; a still becomes a 4 s loop that keeps its alpha with
# hap-alpha or hap-q-alpha. HAP files are several times larger than the H.264 transcodes but need no hardware decoder.
#
# usage: tools/transcode-media.sh COMPOSITION.avc OUT_DIR      runs ffmpeg at nice 15, one file at a time
#   CRF (default 18), PRESET (default slow)
#   HAP (unset: H.264), HAP_CHUNKS (default 1), HAP_ENCODER (default build/nocturne-hap-encode)
set -euo pipefail

AVC=${1:?usage: tools/transcode-media.sh COMPOSITION.avc OUT_DIR}
OUT=${2:?usage: tools/transcode-media.sh COMPOSITION.avc OUT_DIR}
CRF=${CRF:-18}
PRESET=${PRESET:-slow}
HAP=${HAP:-}
HAP_CHUNKS=${HAP_CHUNKS:-1}
HAP_ENCODER=${HAP_ENCODER:-$(dirname "$0")/../build/nocturne-hap-encode}
case $HAP in
    ''|hap|hap-alpha|hap-q|hap-q-alpha) ;;
    *) echo "HAP must be hap, hap-alpha, hap-q or hap-q-alpha" >&2; exit 2 ;;
esac
if [[ -n $HAP && ! -x $HAP_ENCODER ]]; then
    echo "no HAP encoder at $HAP_ENCODER: build it (cmake --build build --target nocturne-hap-encode) or set HAP_ENCODER" >&2
    exit 2
fi
mkdir -p "$OUT"

probe() {
    ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,width,height,avg_frame_rate -of csv=p=0 "$1" </dev/null
}

encode() {
    local source=$1 target=$2 fps=$3
    shift 3
    nice -n 15 ffmpeg -nostdin -hide_banner -loglevel error -y "$@" -i "$source" -an \
        -vf "scale=w='min(1920,iw)':h='min(1080,ih)':force_original_aspect_ratio=decrease,format=yuv420p" \
        -c:v libx264 -preset "$PRESET" -crf "$CRF" -profile:v high -g "$fps" -keyint_min "$fps" -sc_threshold 0 \
        -r "$fps" -tag:v avc1 -movflags +faststart "$target.part.mp4"
    mv "$target.part.mp4" "$target"
}

still() {
    local source=$1 target=$2
    nice -n 15 ffmpeg -nostdin -hide_banner -loglevel error -y -loop 1 -t 4 -framerate 30 -i "$source" -an \
        -filter_complex "color=black:s=1920x1080:r=30:d=4[bg];[0:v]scale=1920:1080:force_original_aspect_ratio=decrease[fg];[bg][fg]overlay=(W-w)/2:(H-h)/2:format=auto,format=yuv420p" \
        -c:v libx264 -preset "$PRESET" -crf "$CRF" -profile:v high -g 30 -keyint_min 30 -sc_threshold 0 -tag:v avc1 \
        -movflags +faststart "$target.part.mp4"
    mv "$target.part.mp4" "$target"
}

# hap SOURCE TARGET WIDTH HEIGHT RATE [ffmpeg input options]: at most 1080p in even sizes, as encode() scales.
hap() {
    local source=$1 target=$2 width=$3 height=$4 rate=$5
    shift 5
    read -r w h < <(awk -v w="$width" -v h="$height" 'BEGIN {
        s = 1; if (w > 1920) s = 1920 / w; if (h * s > 1080) s = 1080 / h
        printf "%d %d\n", int(w * s / 2 + 0.5) * 2, int(h * s / 2 + 0.5) * 2 }')
    nice -n 15 ffmpeg -nostdin -hide_banner -loglevel error "$@" -i "$source" -an -vf "scale=$w:$h" \
        -f rawvideo -pix_fmt rgba - |
        nice -n 15 "$HAP_ENCODER" --size "${w}x$h" --fps "$rate" --codec "$HAP" --chunks "$HAP_CHUNKS" "$target.part.mov"
    mv "$target.part.mov" "$target"
}

copied=0 encoded=0 kept=0 missing=0
while IFS= read -r source; do
    if [[ ! -f $source ]]; then
        echo "missing: $source" >&2
        missing=$((missing + 1))
        continue
    fi
    name=$(basename "$source")
    stem=${name%.*}
    IFS=, read -r codec width height rate < <(probe "$source")
    fps=$(( ${rate%/*} / ${rate#*/} ))
    case $codec in
        png|mjpeg|tiff|bmp|webp) kind=still ;;
        h264|hevc|prores|hap) (( width <= 1920 && height <= 1080 )) && kind=copy || kind=encode ;;
        *) kind=encode ;;
    esac
    if [[ -n $HAP && ( $codec != hap || $kind != copy ) ]]; then
        [[ $kind == still ]] && kind=hap-still || kind=hap
    fi
    if [[ $kind == copy ]]; then
        target="$OUT/$name"
        if [[ -e $target && $(stat -f %z "$target") != $(stat -f %z "$source") ]]; then
            echo "name clash: $target differs from $source" >&2
            exit 1
        fi
        [[ -e $target ]] && kept=$((kept + 1)) && continue
        cp -c "$source" "$target" 2>/dev/null || cp "$source" "$target"
        copied=$((copied + 1))
        continue
    fi
    target="$OUT/$stem.mp4"
    [[ $kind == hap* ]] && target="$OUT/$stem.mov"
    [[ -e $target ]] && kept=$((kept + 1)) && continue
    echo "$kind: $name ($codec ${width}x$height ${fps} fps) -> $(basename "$target")"
    started=$SECONDS
    case $kind in
        still) still "$source" "$target" ;;
        hap) hap "$source" "$target" "$width" "$height" "$rate" ;;
        hap-still) hap "$source" "$target" "$width" "$height" 30 -loop 1 -t 4 -framerate 30 ;;
        *) encode "$source" "$target" "$fps" ;;
    esac
    echo "  $(( SECONDS - started )) s, $(( $(stat -f %z "$target") / 1048576 )) MB"
    encoded=$((encoded + 1))
done < <(grep -o 'VideoFile value="[^"]*"' "$AVC" | sed 's/^VideoFile value="//; s/"$//' | sed 's/&amp;/\&/g' | sort -u)

echo "transcoded $encoded, copied $copied, kept $kept, missing $missing -> $OUT"
