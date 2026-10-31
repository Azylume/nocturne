#!/bin/sh
# Regenerates the test clips with Homebrew ffmpeg (a tool for making assets only; nothing links it).
#
# count_709_tv.mp4 and count_601_pc.mp4: 320x180, 30 fps, 60 frames, H.264 long GOP (one keyframe, B-frames).
# count_100.mp4: the same at 100 frames with a keyframe every 30, Rec.709 video range: 3.33 s, so 8 beats at 144 BPM.
# count_240.mp4: 240 frames with a keyframe every 60 (four long GOPs): 8 s, so 16 beats of 15 frames at 120 BPM.
# count_240_gop240.mp4: the same as one GOP, so reaching a late frame decodes most of the clip.
# Top half: the frame index as 8 bits in 40-pixel columns, most significant bit left, white = 1.
# Bottom half: two solid patches, RGB (200, 40, 40) left and (40, 160, 80) right, to check the colour conversion.
# The first is tagged Rec.709 video range, the second BT.601 full range.
# count_hevc.mp4: count_709_tv.mp4's picture as HEVC Main with B-frames and a keyframe every 30 frames.
# count_prores.mov: the same as ProRes 422 Proxy (10-bit 4:2:2, every frame a keyframe).
set -eu
cd "$(dirname "$0")"

bit='255*mod(floor(N/pow(2,7-floor(X/40))),2)'
picture="format=rgb24,geq=r='if(lt(Y,90),$bit,if(lt(X,160),200,40))':g='if(lt(Y,90),$bit,if(lt(X,160),40,160))':b='if(lt(Y,90),$bit,if(lt(X,160),40,80))'"

# count MATRIX RANGE TAG FRAMES GOP OUT
count() {
    ffmpeg -v error -y -f lavfi -i "color=black:size=320x180:rate=30" -frames:v "$4" \
        -vf "$picture,scale=out_color_matrix=$1:out_range=$2,format=yuv420p" \
        -c:v libx264 -preset slow -crf 16 -g "$5" -keyint_min "$5" -sc_threshold 0 -bf 2 -x264-params b-adapt=0 \
        -colorspace "$3" -color_primaries "$3" -color_trc "$3" -color_range "$2" "$6"
}

# recount PIXEL_FORMAT OUT CODEC_OPTIONS...: 60 frames of the Rec.709 video range picture in another codec.
recount() {
    format=$1
    out=$2
    shift 2
    ffmpeg -v error -y -f lavfi -i "color=black:size=320x180:rate=30" -frames:v 60 \
        -vf "$picture,scale=out_color_matrix=bt709:out_range=tv,format=$format" "$@" \
        -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv "$out"
}

count bt709 tv bt709 60 60 count_709_tv.mp4
count bt601 pc smpte170m 60 60 count_601_pc.mp4
count bt709 tv bt709 100 30 count_100.mp4
count bt709 tv bt709 240 60 count_240.mp4
count bt709 tv bt709 240 240 count_240_gop240.mp4
recount yuv420p count_hevc.mp4 -c:v libx265 -preset slow -crf 16 -tag:v hvc1 \
    -x265-params log-level=error:keyint=30:min-keyint=30:scenecut=0:bframes=2:open-gop=0
recount yuv422p10le count_prores.mov -c:v prores_ks -profile:v proxy -movflags +write_colr

half() { head -c "$(($(wc -c < "$1") / 2))" "$1"; }

# Downloads cut off halfway. Without faststart the index (moov) at the end is missing; with it, the index survives
# but lists samples past the end of the file.
half count_709_tv.mp4 > truncated.mp4
ffmpeg -v error -y -i count_709_tv.mp4 -c copy -movflags +faststart faststart.tmp.mp4
half faststart.tmp.mp4 > truncated_faststart.mp4
rm faststart.tmp.mp4

# A file AVFoundation opens but that has no video to play.
ffmpeg -v error -y -f lavfi -i "sine=frequency=440:duration=0.5" -c:a aac -b:a 32k audio_only.m4a

./unusual_media.sh
