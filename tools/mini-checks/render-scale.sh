#!/bin/bash
# Render scale (127 step 1) on the show as it plays: alternating windows at 100 % and at each lower scale, steady seconds
# only, the median of the paired GPU differences and the outputs' frame rate. The scale goes back to what it was. Not in
# run-all.sh; about 3 minutes.
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"
parse_args "$@"
say "== render scale"

SCALE_ROUNDS=${SCALE_ROUNDS:-3}
SCALE_SECONDS=${SCALE_SECONDS:-10}
SCALE_SETTLE=${SCALE_SETTLE:-3}

was=$(field .renderScale "$(ctl_reply settings settings)")
[[ $was == - ]] && was=100

for scale in 83 67; do
    pairs=() diffs=() fps=() news=()
    for r in $(seq 1 "$SCALE_ROUNDS"); do
        ctl settings render-scale 100 > /dev/null
        pause "$SCALE_SETTLE"
        a=$(steady_window "scale-100-$scale-$r" "$SCALE_SECONDS")
        ctl settings render-scale "$scale" > /dev/null
        pause "$SCALE_SETTLE"
        b=$(steady_window "scale-$scale-$r" "$SCALE_SECONDS")
        full=$(field .gpuMs "$a") scaled=$(field .gpuMs "$b")
        pairs+=("$full/$scaled")
        [[ $full != - && $scaled != - ]] && diffs+=("$(awk -v a="$full" -v b="$scaled" 'BEGIN { printf "%+.2f", b - a }')")
        fps+=("$(field .fps "$b")")
        news+=("$(field '[.outputs[].newMin] | min' "$b")")
    done
    m=$(median "${diffs[@]}")
    row "127/1 render scale $scale %" \
        "nocturne-ctl settings render-scale 100|$scale; $SCALE_ROUNDS rounds of ${SCALE_SECONDS} s, sizes.fading 0" \
        "median ${m:--} ms GPU (100/$scale ${pairs[*]:-}; paired ${diffs[*]:-}), fps ${fps[*]:-}, outputs min new/s ${news[*]:-}" \
        "< 0 ms, 60 fps" "$(pass_if "$m" '<' 0)"
done
ctl settings render-scale "$was" > /dev/null

manual "127/1 render scale by eye" "nocturne-ctl settings render-scale 83, then 67, then back" \
    "the picture softens a little, no edge or seam; L5 (rendered-text video) scales too: compare its shimmer with 100 %"
