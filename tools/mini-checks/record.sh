#!/bin/bash
# Recording (19, 40, 102): the disk reserve state, a 15 s recording with the current settings (output frames and
# recorded frames dropped, the file finalized and playable, with its audio track), and a rollover with max-length 5 (the setting goes back to
# its value). The movies stay where the recorder put them; the table names them, delete them by hand.
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"
parse_args "$@"
say "== recording"

probe() {
    if command -v ffprobe > /dev/null; then
        ffprobe -v error -select_streams v:0 -count_packets -show_entries stream=codec_name,nb_read_packets:format=duration \
            -of csv=p=0 "$1" 2> /dev/null | tr '\n' ' '
    else
        "$HERE/movinfo.pl" "$1"
    fi
}

# "CODEC CHANNELS RATE" of the first audio track, nothing when there is none.
audio_track() {
    if command -v ffprobe > /dev/null; then
        ffprobe -v error -select_streams a:0 -show_entries stream=codec_name,channels,sample_rate -of default=nw=1 "$1" 2> /dev/null |
            awk -F= '{ v[$1] = $2 } END { if (v["codec_name"] != "") print v["codec_name"], v["channels"], v["sample_rate"] }'
    else
        "$HERE/movinfo.pl" "$1" | cut -s -d' ' -f4-
    fi
}

# playable FILE AUDIO: PASS or FAIL and what the file holds; AUDIO is the record state's audio; only true or "on" wants a track.
playable() {
    local file=$1 audio=$2 info video track
    info=$(probe "$file")
    if command -v ffprobe > /dev/null; then
        [[ -n $info ]] || { echo "FAIL ffprobe: unreadable"; return; }
        video="ffprobe: $info"
    else
        set -- $info
        [[ $1 != unfinished ]] && num "$1" '>' 0 && num "$2" '>=' 1 || { echo "FAIL $info"; return; }
        video="$1 s, $2 video track"
    fi
    track=$(audio_track "$file")
    if [[ -n $track ]]; then
        echo "PASS $video, audio $track"
    elif [[ $audio == true || $audio == on ]]; then
        echo "FAIL $video, no audio track although audio is $audio"
    else
        echo "PASS $video, no audio (audio $audio)"
    fi
}

s0=$(ctl_reply record record state)
if [[ $(field .recording "$s0") == true ]]; then
    row "19 recording" "nocturne-ctl record state" "already recording" "not recording" SKIP
    exit 0
fi
from=$(now)
ctl record start > /dev/null
pause 5
during=$(ctl_reply record record state)
row "102 disk reserve" "nocturne-ctl record state (recording)" \
    "space $(field .space "$during"), free $(field .freeMB "$during") MB, stops at $(field .stopsAtMB "$during") MB" \
    "space ok (free above twice the reserve)" "$([[ $(field .space "$during") == ok ]] && echo PASS || echo FAIL)"
pause 10
stopped=$(ctl_reply record record stop)
stats=$(summarize "$(measure_since record "$from")")
file=$(field .file "$stopped")
[[ $file == - || -z $file ]] && file=$(field .file "$during")
row "19 recording frames" "nocturne-ctl record start; state; stop" \
    "$(field .codec "$during") $(field .pixels "$during"), $(field .frames "$during") frames at 5 s, $(field .recordDropped "$stats") dropped, hardware $(field .hardware "$during")" \
    "0 recorded frames dropped" "$(pass_if "$(field .recordDropped "$stats")" '<=' 0)"
hitches=$(jq '[.outputs[].hitches] | add // 0' <<< "$(obj "$stats")" 2> /dev/null)
row "40/19 outputs while recording" "stats.jsonl outputs[].hitches, render.fps" \
    "$hitches hitches, render $(field .fps "$stats") fps" "0 hitches, >= 59.5 fps" \
    "$([[ ${hitches:-1} == 0 ]] && num "$(field .fps "$stats")" '>=' 59.5 && echo PASS || echo FAIL)"
if ((DRY)); then
    row "19 file playable" "ffprobe or movinfo.pl FILE" "-" "finalized, video track, duration > 0, audio track when audio is on" SKIP
elif [[ -f $file ]]; then
    p=$(playable "$file" "$(field .audio "$during")")
    row "19 file playable" "ffprobe or movinfo.pl" "${p#* } ($file)" "finalized, video track, duration > 0, audio track when audio is on" "${p%% *}"
else
    row "19 file playable" "record state .file" "no file at '$file'; stop said $(field .file "$stopped")" "a movie" FAIL
fi

# --- rollover (102): max-length 5 for a 12 s recording, then the old value back
old=$(field .maxSeconds "$s0")
if [[ $old == - ]] && ((!DRY)); then
    row "102 rollover" "nocturne-ctl record max-length" "this build has no max-length" "102 merged" SKIP
else
    ctl record max-length 5 > /dev/null
    : > "$OUT/rollover.marker"
    ctl record start > /dev/null
    pause 12
    ctl record stop > /dev/null
    ctl record max-length "${old/-/0}" > /dev/null
    back=$(field .maxSeconds "$(ctl_reply record record state)")
    dir=$(field .path "$s0")
    [[ -f $file ]] && dir=$(dirname "$file")
    [[ $dir == *.mov ]] && dir=$(dirname "$dir")
    n=0
    ((DRY)) || n=$(find "$dir" -maxdepth 1 -name '*.mov' -newer "$OUT/rollover.marker" 2> /dev/null | wc -l | tr -d ' ')
    result=$(pass_if "$n" '>=' 2)
    ((DRY)) && result=SKIP
    row "102 rollover files" "record max-length 5; start; 12 s; stop" "$n files in $dir" ">= 2 numbered files" "$result"
    row "102 max-length restored" "nocturne-ctl record max-length $old" "$back" "$old" \
        "$([[ $back == "$old" ]] && echo PASS || echo FAIL)"
fi
manual "102 reserve stop" "a recording on a nearly full volume (a disk image), record path on it" \
    "it stops at the reserve with one notice and a playable file; the outputs show no hitch"
