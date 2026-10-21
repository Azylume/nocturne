#!/bin/bash
# Displays and outputs as the engine reports them (28b6803, 7677d46, 06/27): every output attached, no mirroring, the
# unmirroring on, every screen on an attached display, and the engine flags that hide the cursor. Changes nothing;
# saves `nocturne-ctl displays` to the results folder for the before/after of the replug by hand. The corner targets
# found at start are put back on exit, whatever the by-hand rows left on a display.
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"
label=
if [[ ${1:-} == --save ]]; then
    label=${2:?--save LABEL}
    shift 2
fi
parse_args "$@"
say "== displays"

list=$(ctl_reply outputs displays)
echo "$list" > "$OUT/displays-${label:-now}.json"
if [[ -n $label ]]; then
    row "28b6803 displays $label" "nocturne-ctl displays" \
        "$(jq -r '[.displays[] | .id] | join(", ")' <<< "$(obj "$list")" 2> /dev/null)" "compare before/after the replug" INFO
    exit 0
fi

targets=$(jq -r '.outputs[] | [.display, .target[0], .target[1]] | map(. // "") | @tsv' <<< "$(obj "$list")" 2> /dev/null)
restore_targets() {
    local display x y
    while IFS=$'\t' read -r display x y; do
        [[ -n $display ]] || continue
        if [[ -n $x ]]; then act "$CTL" target "$display" "$x" "$y"; else act "$CTL" target "$display" off; fi
    done <<< "$targets"
}
trap restore_targets EXIT

parked=$(jq -r '[.outputs[] | select(.attached | not) | .display] | join(", ")' <<< "$(obj "$list")" 2> /dev/null)
total=$(jq '.outputs | length' <<< "$(obj "$list")" 2> /dev/null)
row "06 outputs attached" "nocturne-ctl displays" "${total:-0} outputs${parked:+, parked: $parked}" "all attached, 2 (projector, TV)" \
    "$([[ -z $parked && ${total:-0} -ge 2 ]] && echo PASS || echo FAIL)"
mirrored=$(jq -r '[.displays[] | select(.mirrorOf) | "\(.id) of \(.mirrorOf)"] | join(", ")' <<< "$(obj "$list")" 2> /dev/null)
row "28b6803 no mirroring" "nocturne-ctl displays .displays[].mirrorOf" "${mirrored:-none}" "none" \
    "$([[ -z $mirrored && -n $list ]] && echo PASS || echo FAIL)"
row "28b6803 unmirror on" "nocturne-ctl displays .unmirror" "$(field .unmirror "$list")" "true" \
    "$([[ $(field .unmirror "$list") == true ]] && echo PASS || echo FAIL)"
lost=$(snapshot | jq -r --argjson l "$(obj "$list")" \
    '[$l.displays[]?.id] as $ids | [.composition.screens[] | select(.display != "" and .display != null and ((.display as $d | $ids | index($d)) | not)) | "\(.name) on \(.display)"] | join(", ")' 2> /dev/null)
row "06 screens on attached displays" "snapshot .composition.screens[].display" "${lost:-all attached}" "all attached" \
    "$([[ -z $lost && -n $list ]] && echo PASS || echo FAIL)"
flags=$(plist_args "$PLIST" | grep -E '^--(windowed|virtual)$' | tr '\n' ' ')
row "7677d46 cursor can hide" "autostart plist arguments" "${flags:-no --windowed or --virtual}" \
    "borderless fullscreen outputs" "$([[ -z $flags ]] && echo PASS || echo FAIL)"

manual "7677d46 cursor" "move the mouse onto the projector and the TV" "no cursor over either"
manual "28b6803 mirroring" "System Settings > Displays: mirror the TV to the projector" \
    "within a few seconds the engine ends it, logs it, the UI shows the notice; each screen back on its display"
manual "28b6803 replug" "displays.sh --save before; unplug the projector (through the USB-C splitter), plug it back; displays.sh --save after" \
    "the status bar flags it missing, then the screen takes it back (the log names the new id); the TV never drops a frame"
manual "26 power order" "power the projector off and on, then the TV" "screens find their displays in either order"
