#!/bin/sh
# Unusual test clips made from count_709_tv.mp4 with Homebrew ffmpeg (make_media.sh runs it last).
#
# rotated.mp4: the same stream with phone-style rotation metadata, shown turned 90 degrees clockwise.
# upright.mp4: that picture turned for real (180x320), the reference rotated.mp4 must look like.
# count_vfr.mp4: variable frame rate, frame n at (n + 0.4 (n mod 3)) / 30 s.
# count_gap.mp4: frames 10-19 missing and the timestamps of the others kept, a third of a second without a frame.
set -eu
cd "$(dirname "$0")"

reencode() {
    ffmpeg -v error -y -i count_709_tv.mp4 -vf "$1" -fps_mode passthrough -video_track_timescale 30000 \
        -c:v libx264 -preset slow -crf 16 -g 60 -bf 2 -colorspace bt709 -color_primaries bt709 -color_trc bt709 \
        -color_range tv "$2"
}

ffmpeg -v error -y -display_rotation -90 -i count_709_tv.mp4 -c copy rotated.mp4
ffmpeg -v error -y -i rotated.mp4 -c:v libx264 -preset slow -crf 16 -bf 0 -colorspace bt709 -color_primaries bt709 \
    -color_trc bt709 -color_range tv upright.mp4
reencode "setpts='(N+0.4*mod(N,3))/30/TB'" count_vfr.mp4
reencode "select='not(between(n,10,19))'" count_gap.mp4
