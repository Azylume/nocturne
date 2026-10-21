#!/bin/bash
# Engine flag A/Bs on the running show (tickets 34 cut 5, 35, 34 cuts 2-7): each side restarts the engine through the
# autostart plist with and without the flag, waits WARMUP s and measures AB_SECONDS s from stats.jsonl. The original
# plist is restored and loaded at the end, also on Ctrl-C. Each restart blanks the outputs for a few seconds.
#   --opaque-outputs   presents/s and GPU ms per output, render GPU ms
#   --realtime-frames  engine CPU, CPU speed limit (pmset -g therm), hitches per output, ctl round trip
#   --gpu-profile      one capture for bench/gpu_profile_report.py on the laptop (per-layer stripper and L5 passes)
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"
parse_args "$@"
say "== flag A/Bs (results in $OUT)"

if ((!RESTARTS)); then
    row "34/35 flag A/Bs" "ab-flags.sh" "-" "needs engine restarts" SKIP
    exit 0
fi
if ! engine_running || { ((!DRY)) && [[ ! -f $PLIST ]]; }; then
    row "34/35 flag A/Bs" "ab-flags.sh" "no autostart agent" "the show runs under autostart" SKIP
    exit 0
fi

plist_backup
trap plist_restore EXIT
trap 'exit 130' INT TERM

speed_limit_min() {
    local min=100 v
    while IFS= read -r v; do
        [[ -n $v ]] && ((v < min)) && min=$v
    done < "$1"
    echo "$min"
}

# Samples pmset -g therm every 5 s while the window runs, then measures. Prints the stats file.
measure_with_therm() {
    local name=$1 therm=$OUT/therm-$1.txt
    : > "$therm"
    if ((!DRY)); then
        (for _ in $(seq 1 $((AB_SECONDS / 5))); do
            pmset -g therm | awk -F= '/CPU_Speed_Limit/ {gsub(/[ \t]/, "", $2); print $2}' >> "$therm"
            sleep 5
        done) &
    fi
    measure "$name" "$AB_SECONDS"
    wait 2> /dev/null
}

ctl_round_trip_max() {
    local i t0 t1 max=0
    for i in 1 2 3 4 5; do
        t0=$(now_ms)
        ctl clock > /dev/null
        t1=$(now_ms)
        (((t1 - t0) > max)) && max=$((t1 - t0))
    done
    echo "$max"
}

side() {
    local flag=$1 with=$2 name=$3 plist
    plist=$(plist_variant "$flag" "$with" "$name") || { say "cannot write the $name plist"; return 1; }
    run_with_plist "$plist" || return 1
    pause "$WARMUP"
    rtt[$with]=$(ctl_round_trip_max)
    measure_with_therm "$name" > /dev/null
    sum[$with]=$(summarize "$OUT/stats-$name.jsonl")
}

outputs_line() {
    jq -r '[.outputs[] | "\(.display): \(.presents)/s, gpu \(.gpuMs) ms, \(.hitches) hitches"] | join("; ")' <<< "$1"
}

# --- --opaque-outputs (34 cut 5)
sum=() rtt=()
if side --opaque-outputs 0 opaque-off && side --opaque-outputs 1 opaque-on; then
    off=${sum[0]} on=${sum[1]}
    row "34/5 opaque off: outputs" "plist without --opaque-outputs" "$(outputs_line "$off")" "reference" INFO
    row "34/5 opaque on: outputs" "plist with --opaque-outputs" "$(outputs_line "$on")" "reference" INFO
    gain=$(jq -rn --argjson a "$off" --argjson b "$on" \
        '[range(0; [$a.outputs | length, ($b.outputs | length)] | min) as $i |
          ($b.outputs[$i].presents - $a.outputs[$i].presents >= -0.5) and ($b.outputs[$i].gpuMs <= $a.outputs[$i].gpuMs + 0.05)]
         | if length == 0 then "none" elif all then "yes" else "no" end')
    result=FAIL
    [[ $gain == yes ]] && result=PASS
    [[ $gain == none ]] && result=SKIP
    row "34/5 opaque outputs gain" "stats.jsonl outputs[].presents, gpuMs" "render gpu $(field .gpuMs "$off") -> $(field .gpuMs "$on") ms" \
        "every output: presents not lower, output GPU ms not higher" "$result"
    manual "34/5 opaque outputs picture" "look at both outputs during the on side" \
        "nothing looks different (masks, soft edges); only then make it the default"
else
    row "34/5 opaque outputs A/B" "ab-flags.sh" "a side failed" "both sides measured" FAIL
fi

# --- --realtime-frames (35)
sum=() rtt=()
if side --realtime-frames 0 realtime-off && side --realtime-frames 1 realtime-on; then
    off=${sum[0]} on=${sum[1]}
    h_off=$(jq '[.outputs[].hitches] | add // 0' <<< "$off")
    h_on=$(jq '[.outputs[].hitches] | add // 0' <<< "$on")
    lim=$(speed_limit_min "$OUT/therm-realtime-on.txt")
    row "35 realtime: engine CPU" "stats.jsonl cpu" "$(field .cpu "$off") -> $(field .cpu "$on") %" "expect +8-10 points" INFO
    row "35 realtime: hitches" "stats.jsonl outputs[].hitches" "$h_off -> $h_on ($(outputs_line "$on"))" "on <= off" \
        "$(pass_if "$h_on" '<=' "$h_off")"
    row "35 realtime: CPU speed limit" "pmset -g therm" "min $lim" "stays at 100" "$(pass_if "$lim" '>=' 100)"
    row "35 realtime: ctl round trip" "nocturne-ctl clock x5" "${rtt[0]} -> ${rtt[1]} ms max" "on < 500 ms" \
        "$(pass_if "${rtt[1]:--}" '<' 500)"
    manual "35 realtime: responsiveness" "laptop UI and audio during the on side" \
        "faders, deck switches and audio reactions stay immediate; flip the default only if all four rows pass"
else
    row "35 realtime frames A/B" "ab-flags.sh" "a side failed" "both sides measured" FAIL
fi

# --- --gpu-profile capture (34 cuts 2-7)
sum=() rtt=()
if side --gpu-profile 1 gpu-profile; then
    row "34 gpu-profile capture" "plist with --gpu-profile" "$OUT/stats-gpu-profile.jsonl" \
        "copy to the laptop: bench/gpu_profile_report.py FILE --passes" INFO
fi

plist_restore
trap - EXIT
