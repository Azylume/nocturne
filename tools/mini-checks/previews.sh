#!/bin/bash
# Preview load on the Mini (62, 7362b70): render.previews.gpuMs and watchers in stats.jsonl as the show runs now, then
# with one remote preview watch open (nocturne-ctl clippreview, as a UI without local previews does). Clip previews
# are machine-local and off by default; the watch turns them on for its 25 s and back off after, if they were off.
#   previews.sh --sample LABEL [options]   only measure 20 s and print one row, for the laptop UI A/B by hand:
#                                          LABEL local (UI as is), remote (UI started with --remote-previews), off
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"
label=
if [[ ${1:-} == --sample ]]; then
    label=${2:?--sample LABEL}
    shift 2
fi
parse_args "$@"

preview_row() {
    local name=$1 s=$2 threshold=$3 result=$4
    row "$name" "stats.jsonl render.previews" \
        "previews $(field .previewsMs "$s") ms, watchers $(field .watchersMax "$s"), fps $(field .fps "$s")" "$threshold" "$result"
}

if [[ -n $label ]]; then
    s=$(summarize "$(measure "previews-$label" 20)")
    preview_row "62 previews: laptop UI $label" "$s" "local or off: about 0 ms, 0 watchers; remote: the before number" INFO
    exit 0
fi

say "== previews"
now_s=$(summarize "$(measure previews-now 20)")
w=$(field .watchersMax "$now_s")
if [[ $w == 0 ]]; then
    preview_row "62 previews now" "$now_s" "0 watchers: about 0 ms" "$(pass_if "$(field .previewsMs "$now_s")" '<=' 0.2)"
else
    preview_row "62 previews now" "$now_s" "watchers > 0: a UI with remote previews or media the laptop lacks" INFO
fi

cell=$(snapshot | jq -r '[.composition | .. | objects | select(has("blend") and .playing != null and .clip != null)]
    | first | if . == null then empty else "\(.id) \(.playing.column)" end' 2> /dev/null)
if [[ -z $cell ]]; then
    row "62 preview watch" "nocturne-ctl clippreview" "no playing clip" "a playing clip" SKIP
else
    set -- $cell
    clips=$( ((DRY)) || "$CTL" clippreviews 2> /dev/null | jq -r 'select(.type == "preview" and .op == "clips") | .on')
    [[ $clips == true ]] || act "$CTL" clippreviews on > /dev/null
    if ((DRY)); then
        say "[dry-run] nocturne-ctl clippreview $1 $2 30 &"
    else
        "$CTL" clippreview "$1" "$2" 30 > "$OUT/clippreview.txt" 2>&1 &
    fi
    pause 5
    with=$(summarize "$(measure previews-watch 20)")
    wait 2> /dev/null
    [[ $clips == true ]] || act "$CTL" clippreviews off > /dev/null
    w1=$(field .watchersMax "$with")
    result=FAIL
    [[ $w1 != - ]] && num "$w1" '>' "$w" && num "$(field .fps "$with")" '>=' 59.5 && result=PASS
    preview_row "62 one ctl preview watch (layer $1 column $2)" "$with" "watchers rise, render stays >= 59.5 fps" "$result"
fi

manual "62 laptop UI local vs remote" "previews.sh --sample local; restart the UI with --remote-previews; --sample remote" \
    "local: about 0 ms and 0 watchers with every monitor showing; remote: the before number (1.5-5 ms)"
manual "62 laptop UI load" "Activity Monitor on the laptop, every monitor showing, local vs --remote-previews" \
    "the UI stays responsive; with Previews off its GPU time drops to about 0"
manual "f3af831 Previews switch" "top bar Previews off, then on, then restart the UI" \
    "every monitor and the Map say Previews off; they come back; the setting survives the restart"
