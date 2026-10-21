#!/bin/bash
# Generates the odd-media corpus with Homebrew ffmpeg: broken and unusual files every build must play or refuse
# without a crash or a render-thread stall (tests/resilience_test.cpp, "the odd-media corpus"). Deterministic and not
# committed: the files go to a folder named after this script's hash, with manifest.json saying what each one must do
# ("play", "flag" or "either"). Prints that folder.
#
# Everything is synthetic, phone-style clips included: rotation metadata, HEVC in QuickTime with an AAC track and
# jittered (variable) frame timing, as iPhones write them, but generated here.
#
# usage: make_odd_media.sh [CACHE_DIR]   (default ~/Library/Caches/Nocturne/odd-media)
set -euo pipefail

root=${1:-$HOME/Library/Caches/Nocturne/odd-media}
out="$root/$(shasum "$0" | cut -c1-12)"
[[ -s $out/manifest.json ]] && { echo "$out"; exit 0; }
mkdir -p "$out"
cd "$out"
manifest=()

expect() {
    manifest+=("\"$1\": {\"expect\": \"$2\", \"what\": \"$3\"}")
}

# enc NAME EXPECT WHAT SIZE FPS SECONDS [ffmpeg output options...]: a moving test pattern with a frame counter.
enc() {
    local name=$1 expected=$2 what=$3 size=$4 fps=$5 seconds=$6
    shift 6
    nice -n 15 ffmpeg -v error -y -f lavfi -i "testsrc2=s=$size:r=$fps" -t "$seconds" -map_metadata -1 \
        -fflags +bitexact -flags +bitexact "$@" "$name"
    expect "$name" "$expected" "$what"
}

x264() {
    echo -c:v libx264 -preset fast -crf 20 -threads 4 -pix_fmt "${1:-yuv420p}"
}

# A deterministic byte pattern, not zeros: N bytes.
garbage() {
    perl -e 'print chr(($_ * 7919 + 13) % 251) for 1..$ARGV[0]' "$1"
}

enc reference_1080p30.mp4 play "H.264 1080p30, the reference" 1920x1080 30 4 $(x264)

cp reference_1080p30.mp4 corrupt_middle.mp4
garbage 65536 | dd of=corrupt_middle.mp4 bs=1 seek=$(($(stat -f%z corrupt_middle.mp4) / 2)) conv=notrunc 2> /dev/null
expect corrupt_middle.mp4 either "64 KB of garbage in the middle of the sample data"

cp reference_1080p30.mp4 corrupt_header.mp4
garbage 4096 | dd of=corrupt_header.mp4 bs=1 conv=notrunc 2> /dev/null
expect corrupt_header.mp4 flag "the first 4 KB overwritten"

head -c $(($(stat -f%z reference_1080p30.mp4) / 2)) reference_1080p30.mp4 > truncated_index_lost.mp4
expect truncated_index_lost.mp4 flag "cut off halfway, index (moov) at the end lost"

nice -n 15 ffmpeg -v error -y -i reference_1080p30.mp4 -c copy -movflags +faststart -fflags +bitexact faststart.tmp.mp4
head -c $(($(stat -f%z faststart.tmp.mp4) / 3)) faststart.tmp.mp4 > truncated_faststart.mp4
command rm -f faststart.tmp.mp4
expect truncated_faststart.mp4 flag "a third of a faststart file: the index lists samples past the end"

: > zero_bytes.mp4
expect zero_bytes.mp4 flag "an empty file"
printf 'this is not a video\n%.0s' {1..200} > not_video.mp4
expect not_video.mp4 flag "text named .mp4"

enc vfr_jitter.mov play "variable frame rate, timestamps jittered by up to 13 ms" 1280x720 30 4 \
    -vf "setpts='N/30/TB+0.013*sin(N*1.7)/TB'" -fps_mode passthrough -video_track_timescale 600 $(x264)
enc vfr_drops.mp4 play "variable frame rate, a third of the frames dropped in bursts" 1280x720 30 4 \
    -vf "select='lt(mod(n,30),20)'" -fps_mode passthrough $(x264)

for turn in 90 180 270; do
    enc "rotated_$turn.mp4" play "rotation metadata, shown turned $turn degrees clockwise" 1280x720 30 3 $(x264)
    # An input option: the metadata goes on in a second, copying pass.
    nice -n 15 ffmpeg -v error -y -display_rotation "-$turn" -i "rotated_$turn.mp4" -c copy -fflags +bitexact \
        "rot.tmp.mp4"
    mv rot.tmp.mp4 "rotated_$turn.mp4"
done

nice -n 15 ffmpeg -v error -y -f lavfi -i "testsrc2=s=1920x1080:r=30" -f lavfi -i "sine=frequency=330:sample_rate=48000" \
    -t 4 -map_metadata -1 -fflags +bitexact -flags +bitexact \
    -vf "setpts='N/30/TB+0.004*sin(N*2.3)/TB'" -fps_mode passthrough -video_track_timescale 600 \
    -c:v libx265 -preset fast -crf 24 -pix_fmt yuv420p -x265-params pools=4:frame-threads=1:log-level=error -tag:v hvc1 \
    -c:a aac -b:a 128k phone.tmp.mov
nice -n 15 ffmpeg -v error -y -display_rotation -90 -i phone.tmp.mov -c copy -fflags +bitexact phone_hevc_portrait.mov
command rm -f phone.tmp.mov
expect phone_hevc_portrait.mov play "phone-style: HEVC 1080p in QuickTime, portrait rotation, AAC, jittered timing"

enc hevc_10bit.mov play "HEVC Main 10, 4:2:0 10-bit" 1920x1080 30 3 \
    -c:v libx265 -preset fast -crf 24 -pix_fmt yuv420p10le -x265-params pools=4:frame-threads=1:log-level=error -tag:v hvc1
enc h264_10bit.mp4 either "H.264 High 10 (no hardware decoder on Macs)" 1920x1080 30 3 $(x264 yuv420p10le)
enc prores_422.mov play "ProRes 422" 1920x1080 30 2 -c:v prores_ks -profile:v 2
enc odd_444_321x241.mp4 either "odd size 321x241, H.264 4:4:4" 320x240 30 3 -vf scale=321:241 $(x264 yuv444p)
enc odd_hevc444_321x241.mov either "odd size 321x241, HEVC 4:4:4" 320x240 30 3 -vf scale=321:241 \
    -c:v libx265 -preset fast -crf 24 -pix_fmt yuv444p -x265-params pools=4:frame-threads=1:log-level=error -tag:v hvc1
enc size_318x178.mp4 play "318x178, not a multiple of 4 or 16" 320x180 30 3 -vf scale=318:178 $(x264)
enc size_1918x1078.mp4 play "1918x1078, not a multiple of 4" 1920x1080 30 3 -vf scale=1918:1078 $(x264)
enc tiny_16x16.mp4 play "16x16" 16x16 30 3 $(x264)
enc tall_1080x1920.mp4 play "portrait 1080x1920 stored as such" 1080x1920 30 3 $(x264)
enc uhd_h264_60.mp4 play "H.264 3840x2160 at 60 fps" 3840x2160 60 3 $(x264) -level 5.2
enc uhd_hevc_30.mov play "HEVC 3840x2160 at 30 fps" 3840x2160 30 3 \
    -c:v libx265 -preset fast -crf 26 -pix_fmt yuv420p -x265-params pools=4:frame-threads=1:log-level=error -tag:v hvc1
enc fps_1.mp4 play "1 fps" 1280x720 1 6 $(x264)
enc fps_23976.mp4 play "23.976 fps" 1280x720 24000/1001 3 $(x264)
enc fps_120.mp4 play "1080p at 120 fps" 1920x1080 120 3 $(x264)
enc interlaced_1080i.mp4 play "1080i29.97, top field first" 1920x1080 30000/1001 3 $(x264) -flags +ildct+ilme -x264-params tff=1
enc gop_600.mp4 play "one keyframe in 20 s" 1280x720 30 20 $(x264) -g 600 -keyint_min 600 -sc_threshold 0
enc vp9.webm flag "VP9 in WebM (unsupported container)" 1280x720 30 2 -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 1M
enc vp9.mp4 either "VP9 in MP4" 1280x720 30 2 -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 1M
enc av1.mp4 either "AV1 (hardware decode only on newer Apple silicon)" 1280x720 30 2 -c:v libsvtav1 -preset 10 -crf 40
enc mpeg2.mpg either "MPEG-2 program stream" 1280x720 30 2 -c:v mpeg2video -q:v 4
enc mpeg4_part2.mp4 either "MPEG-4 Part 2" 1280x720 30 2 -c:v mpeg4 -q:v 4
nice -n 15 ffmpeg -v error -y -f lavfi -i "sine=frequency=440:duration=2" -map_metadata -1 -fflags +bitexact -c:a aac \
    audio_only.m4a
expect audio_only.m4a flag "audio only"

joined=$(printf '%s,\n  ' "${manifest[@]}")
printf '{\n  %s\n}\n' "${joined%,*}" > manifest.json
echo "$out"
