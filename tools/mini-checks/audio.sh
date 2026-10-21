#!/bin/bash
# Audio (77, 38/57/1c0f105, 89): the input in use; the envelope presets' reaction on live input (each `modenv all P` is
# taken back with one undo and the modulators are compared with the start); the input, channels and gain across a
# supervisor restart (`launchctl kickstart -k`, the same as tools/autostart.sh restart); the DJ link state.
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"
parse_args "$@"
say "== audio"

input=$(ctl_reply audio audio input)
row "38 audio input" "nocturne-ctl audio input" \
    "$(field .name "$input"), channels $(jq -c '.channels // []' <<< "$(obj "$input")"), $(field .state "$input"), $(field .permission "$input")" \
    "running (not fallback or none), permission authorized" \
    "$([[ $(field .state "$input") == running ]] && echo PASS || echo FAIL)"

# --- envelope presets (77)
MODS='[.composition | .. | objects | select(.source? == "audio")]'
start=$(snapshot | jq -cS "$MODS" 2> /dev/null)
count=$(jq 'length' <<< "${start:-[]}")
level=$(ctl_reply audio audio state | jq -r 'if .live then .values.level else 0 end' 2> /dev/null)
if [[ ${count:-0} == 0 ]]; then
    row "77 envelope presets" "nocturne-ctl modenv all PRESET" "no audio modulator in the show" "an audio modulator" SKIP
elif ! num "${level:-0}" '>' 0.02 && ((!DRY)); then
    row "77 envelope presets" "nocturne-ctl audio state" "level ${level:-0}" "live music on the input" SKIP
else
    # 6 s of `mod state`: per audio modulator, value range and reversals (dips over 0.01 that turn back up) per second.
    react() {
        local preset=$1 file=$OUT/modstate-$1.jsonl t0
        : > "$file"
        t0=$(now)
        while (($(now) - t0 < 6)); do
            ctl mod state | jq -c 'select(.type == "modulation")' >> "$file"
            ((DRY)) && break
        done
        jq -s --argjson ids "$(jq -c 'map(.id)' <<< "$start")" '
          def rev: [range(1; length - 1) as $i | select(.[$i] < .[$i - 1] - 0.01 and .[$i + 1] > .[$i] + 0.01)] | length;
          (length) as $n |
          [$ids[] as $id | [.[] | .modulators[]? | select(.id == $id) | .value] | select(length > 2)
           | {range: (max - min), rev: (rev / 6)}]
          | {range: (map(.range) | if length == 0 then null else max | . * 100 | round / 100 end), rev: (map(.rev) | if length == 0 then null else add / length | . * 100 | round / 100 end), samples: $n}' \
            "$file"
    }
    results=()
    for p in meter punchy smooth gate; do
        ctl modenv all "$p" > /dev/null
        pause 1
        r=$(react "$p")
        ctl undo > /dev/null
        results+=("$r")
        row "77 $p on live input" "nocturne-ctl modenv all $p; mod state x6 s; undo" \
            "range $(field .range "$r"), $(field .rev "$r") reversals/s ($(field .samples "$r") samples)" "reference" INFO
    done
    meter_range=$(field .range "${results[0]}")
    row "77 Meter reacts" "mod state" "range $meter_range" ">= 0.1" "$(pass_if "$meter_range" '>=' 0.1)"
    row "77 Smooth calmer than Punchy" "mod state" "$(field .rev "${results[2]}") vs $(field .rev "${results[1]}") reversals/s" \
        "smooth <= punchy" "$(pass_if "$(field .rev "${results[2]}")" '<=' "$(field .rev "${results[1]}")")"
    after=$(snapshot | jq -cS "$MODS" 2> /dev/null)
    row "77 modulators restored" "snapshot audio modulators before vs after" \
        "$([[ $after == "$start" ]] && echo same || echo DIFFERENT)" "same" "$([[ $after == "$start" ]] && echo PASS || echo FAIL)"
    [[ $after == "$start" ]] || ((DRY)) || say "WARNING: audio modulators differ; compare $OUT/modulators-before.json"
    echo "$start" > "$OUT/modulators-before.json"
fi
manual "77 presets by ear" "nocturne-ctl modenv all meter (Cmd-Z undoes), then punchy, smooth, breathe, gate" \
    "Meter like the mixer's meter; Punchy pumps; Smooth and Breathe glide; Gate flashes once per kick, dark between"

# --- input, channels and gain across a restart (38, 57, 1c0f105)
if ((!RESTARTS)); then
    row "38/57 input across a restart" "launchctl kickstart -k" "-" "needs an engine restart" SKIP
elif ! engine_running; then
    row "38/57 input across a restart" "launchctl kickstart -k" "no autostart agent" "the show runs under autostart" SKIP
else
    devices="$STATE/audio-devices.json"
    [[ -f $devices ]] && cp "$devices" "$OUT/audio-devices.before.json"
    gain0=$(ctl_reply audio audio state | jq -r '.gainDb // "-"')
    lines=$( (wc -l < "$LOGS/engine.log") 2> /dev/null || echo 0)
    since=$(now)
    act launchctl kickstart -k "$DOMAIN/$LABEL"
    wait_rendering "$since"
    pause 5
    input1=$(ctl_reply audio audio input)
    gain1=$(ctl_reply audio audio state | jq -r '.gainDb // "-"')
    same=$(jq -rn --argjson a "${input:-null}" --argjson b "${input1:-null}" \
        'if $a == null or $b == null then "-" elif $a.uid == $b.uid and $a.channels == $b.channels and $b.state == "running" then "yes" else "no" end')
    row "38 input after kickstart -k" "nocturne-ctl audio input" "$(field .name "$input1") $(jq -c '.channels // []' <<< "$(obj "$input1")") $(field .state "$input1")" \
        "same uid and channels, running" "$([[ $same == yes ]] && echo PASS || echo FAIL)"
    reopened=$(tail -n +"$((lines + 1))" "$LOGS/engine.log" 2> /dev/null | grep -c "reopening" || true)
    row "1c0f105 log says reopening" "grep reopening engine.log" "${reopened:-0} lines" ">= 1" "$(pass_if "${reopened:-0}" '>=' 1)"
    if [[ -f $devices ]] && ((!DRY)); then
        cp "$devices" "$OUT/audio-devices.after.json"
        same_file "$OUT/audio-devices.before.json" "$OUT/audio-devices.after.json" && d=same || d=changed
        row "57 per-device gain and channels" "audio-devices.json before/after" "$d; applied gain $gain0 -> $gain1 dB" \
            "same file" "$([[ $d == same ]] && echo PASS || echo FAIL)"
    fi
fi
manual "42/57 Voyager replug" "unplug the Voyager, wait, plug it back; then set a gain with Auto off and replug again" \
    "it leaves and returns in the UI's Audio list; the same gain and channels come back"
manual "39 Snare macro" "a Snare-driven macro on a Stripper's Dry/wet (new modulator)" "it breathes with the snare, no flicker"

# --- DJ link (89)
dj=$(ctl_reply djlink djlink status)
row "89 DJ link" "nocturne-ctl djlink status" \
    "enabled $(field .enabled "$dj"), interface $(field .interface "$dj"), listening $(field .listening "$dj"), devices $(jq '.devices | length' <<< "$(obj "$dj")" 2> /dev/null), on air $(field .onAir "$dj"), bpm $(field .bpm "$dj")" \
    "with CDJs: on-air player and BPM match the decks" INFO
manual "89 real CDJs" "djlink on en7 (the DJ interface); djlink status during a transition" \
    "BPM, beat in bar and on-air player match; the beat clock follows the on-air CDJ; setting still on after a restart"
manual "89 receive only" "sudo tcpdump -i en7 -w ~/Nocturne/mini-checks-results/djlink.pcap udp portrange 50000-50002 (a few minutes)" \
    "nothing sent by the Mini; no CDJ shows an extra device; keep the pcap for the replay fixtures"
