#!/bin/bash
# Generates the benchmark clips with Homebrew ffmpeg: deterministic, textured, moving 1080p H.264 with B-frames, short
# GOP (keyframe every second) and long GOP, 30 and 60 fps. The clips go to a folder named after this script's hash, so
# a changed script makes a fresh set. Prints that folder.
#
# usage: make_media.sh [CACHE_DIR]   (default ~/Library/Caches/Nocturne/bench-media)
set -euo pipefail

root=${1:-$HOME/Library/Caches/Nocturne/bench-media}
out="$root/$(shasum "$0" | cut -c1-12)"

# clip NAME FPS SECONDS GOP SOURCE
clip() {
    local file="$out/$1.mp4"
    [[ -s $file ]] && return
    nice -n 15 ffmpeg -v error -y -f lavfi -i "$5" -t "$3" \
        -vf "hue=h=t*37:s=1.3,noise=alls=7:allf=t+u:all_seed=$2$4,format=yuv420p" \
        -c:v libx264 -preset medium -crf 18 -maxrate 16M -bufsize 32M -r "$2" -g "$4" -keyint_min "$4" \
        -sc_threshold 0 -bf 3 -threads 4 -an -movflags +faststart "$file.tmp.mp4"
    mv "$file.tmp.mp4" "$file"
}

mkdir -p "$out"
clip jump60_long 60 60 300 "gradients=s=1920x1080:r=60:nb_colors=6:speed=0.01:type=spiral:seed=7,format=rgb24[a];life=s=960x540:r=60:seed=9:ratio=0.35:mold=20:life_color=0xffffff:death_color=0x000000:mold_color=0x606060,scale=1920:1080:flags=bilinear,format=rgb24[b];[a][b]blend=all_mode=multiply"
clip a30_short 30 8 30 "cellauto=s=1920x1080:r=30:rule=110:seed=4"
clip b30_long 30 8 240 "life=s=480x270:r=30:seed=5:mold=10:life_color=0xffd040:death_color=0x2040a0,scale=1920:1080:flags=neighbor"
clip c60_short 60 4 60 "gradients=s=1920x1080:r=60:nb_colors=5:speed=0.03:type=spiral:seed=2"
clip d60_long 60 8 480 "zoneplate=s=1920x1080:r=60:kt2=2:kx2=60:ky2=60:kt=1"
clip e30_short 30 8 30 "mandelbrot=s=1920x1080:r=30:end_scale=0.05"
clip f30_long 30 8 120 "sierpinski=s=1920x1080:r=30:seed=3:jump=20"
echo "$out"
