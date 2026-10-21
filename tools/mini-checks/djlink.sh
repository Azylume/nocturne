#!/bin/bash
# DJ link with the simulator (89, 112): tools/djlink-sim/djlink_sim.py runs on the laptop, broadcasting the built-in
# scenario (about 90 s) on the Mini's network. This script turns the DJ link on on DJ_IF, waits for the start of the
# simulator's scenario, samples `clock` and `djlink status` through it, and records: BPM and bar follow the on-air
# player, the handover bends without a jump, the tempo is held when the link is lost; the engine's UDP sockets (nettop,
# no sudo) and, with sudo, a tcpdump on DJ_IF show the Mini sends nothing on UDP 50000-50002. The DJ link setting goes
# back to what it was. Real CDJs stay MANUAL.
# Environment: DJ_IF (en0) the interface the simulator reaches, SIM_SECONDS (92) seconds sampled, WAIT_SIM (180)
# seconds to wait for the simulator.
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"
parse_args "$@"
DJ_IF=${DJ_IF:-en0}
SIM_SECONDS=${SIM_SECONDS:-92}
WAIT_SIM=${WAIT_SIM:-180}
say "== djlink (simulator)"

djstatus() { ctl_reply djlink djlink status; }

before=$(djstatus)
was_on=$(field .enabled "$before")
was_if=$(field .interface "$before")
restore() {
    if [[ $was_on == true ]]; then
        ctl djlink on "$was_if" > /dev/null
    elif [[ $was_if != - && -n $was_if ]]; then
        ctl djlink on "$was_if" > /dev/null
        ctl djlink off > /dev/null
    else
        ctl djlink off > /dev/null
    fi
}

ip=$( ((DRY)) && echo 192.168.1.10 || ipconfig getifaddr "$DJ_IF")
bcast=$( ((DRY)) && echo 192.168.1.255 || ifconfig "$DJ_IF" 2> /dev/null | awk '/broadcast/ { print $NF; exit }')
if [[ -z $ip ]]; then
    row "112 DJ link simulator" "ipconfig getifaddr $DJ_IF" "no address on $DJ_IF" "DJ_IF on the simulator's network" SKIP
    exit 0
fi

# The capture runs for the whole window: every UDP 50000-50002 packet on DJ_IF, the Mini's own counted afterwards.
pcap=$OUT/djlink.pcap
dump=
if ((DRY)); then
    say "[dry-run] sudo tcpdump -i $DJ_IF -n -w $pcap udp portrange 50000-50002"
elif { [[ -t 0 ]] && sudo -v; } || sudo -n true 2> /dev/null; then
    sudo tcpdump -i "$DJ_IF" -n -w "$pcap" udp portrange 50000-50002 2> "$OUT/tcpdump.err" &
    dump=$!
fi
trap '[[ -n $dump ]] && sudo kill "$dump" 2> /dev/null; restore' EXIT

ctl djlink on "$DJ_IF" > /dev/null
say "start the simulator on the laptop now (the Mini is $ip on $DJ_IF):"
say "  python3 tools/djlink-sim/djlink_sim.py --broadcast ${bcast:-BROADCAST} --interface IF"
# A fresh run: CDJ 2 on air while CDJ 3 is still cued, so the samples cover the whole scenario.
fresh() { jq -e '.onAir == 2 and ([.players[]? | select(.number == 3 and .live)] | length == 0)' > /dev/null 2>&1; }
waited=0
until djstatus | fresh || ((DRY)); do
    ((waited == 20)) && say "waiting for the scenario's start (restart the simulator if it is already running)"
    if ((waited >= WAIT_SIM)); then
        row "112 DJ link simulator" "djlink status" "no fresh scenario within $WAIT_SIM s" "the simulator heard" FAIL
        exit 0
    fi
    sleep 1
    waited=$((waited + 1))
done

# One JSON line per sample: the djlink status and the clock state, each stamped with engine time.
samples=$OUT/djlink-samples.jsonl
if ((DRY)); then
    say "[dry-run] sample djlink status and clock for $SIM_SECONDS s"
    if [[ -n $SAMPLES && -f $SAMPLES/djlink-samples.jsonl ]]; then cp "$SAMPLES/djlink-samples.jsonl" "$samples"; else : > "$samples"; fi
else
    : > "$samples"
    end=$(($(now) + SIM_SECONDS))
    while (($(now) < end)); do
        dj=$(djstatus)
        clock=$(ctl_reply clock clock)
        jq -nc --argjson dj "$(obj "$dj")" --argjson clock "$(obj "$clock")" \
            '{dj: ($dj | {time, onAir, bpm, beat}), clock: ($clock | {time, beat, bpm})}' >> "$samples"
    done
fi

# Per sample: seconds since the on-air player last changed, the player before, the last followed BPM, the sample before.
# The two replies come from separate ctl calls; the on-air beat is carried forward from its status time to the clock's.
result=$(jq -s '
  def wrap: . - 4 * ((. + 2) / 4 | floor);
  def bar: . - 4 * ((. / 4) | floor);
  def r3: . * 1000 | round / 1000;
  [foreach (.[] | select(.dj.onAir != null and .dj.time != null and .clock.beat != null)) as $x
     ({air: -1, since: 0, from: -1, last: null, cur: null};
      (if $x.dj.onAir != .air then .from = .air | .air = $x.dj.onAir | .since = $x.dj.time else . end)
      | .prev = .cur | .cur = $x
      | (if $x.dj.onAir != 0 and $x.dj.bpm != null then .last = $x.dj.bpm else . end);
      $x + {since: (.cur.dj.time - .since), from, last, prev})] as $s
  | ($s | map(select(.dj.onAir != 0 and .dj.bpm != null and .dj.beat != null and .since >= 10))) as $follow
  | ($follow | map(((.clock.beat | bar) - ((.dj.beat.inBar - 1) + .dj.beat.phase
        + (.clock.time - .dj.time) * .dj.bpm / 60)) | wrap | fabs)) as $bars
  | ($s | map(select(.from > 0 and .dj.onAir > 0 and .since < 12 and .prev != null and .dj.bpm != null
        and .clock.time > .prev.clock.time)
      | ((.clock.beat - .prev.clock.beat) / (.clock.time - .prev.clock.time) / (.dj.bpm / 60) - 1 | fabs))) as $bend
  | ($s | map(select(.dj.onAir == 0 and .last != null and .since >= 3))) as $lost
  | ($lost | map(select(.prev != null and .clock.time > .prev.clock.time and .prev.dj.onAir == 0)
      | ((.clock.beat - .prev.clock.beat) / (.clock.time - .prev.clock.time) / (.clock.bpm / 60) - 1 | fabs))) as $run
  | {samples: ($s | length), follow: ($follow | length),
     bpmOk: ($follow | map(select((.clock.bpm - .dj.bpm) | fabs <= 0.05)) | length),
     barOk: ($bars | map(select(. <= 0.15)) | length), barMax: (($bars | max // null) | if . then r3 else . end),
     handovers: ($s | map(select(.from > 0 and .dj.onAir > 0 and .since == 0)) | length),
     bendSamples: ($bend | length), bendMax: (($bend | max // null) | if . then r3 else . end),
     lost: ($lost | length), lastBpm: ($lost[0].last // null),
     heldMax: (($lost | map((.clock.bpm - .last) | fabs) | max // null) | if . then r3 else . end),
     runMax: (($run | max // null) | if . then r3 else . end)}' "$samples" 2> /dev/null)
echo "$result" > "$OUT/djlink-result.json"

ratio_ok() { [[ $1 =~ ^[0-9]+$ && $2 =~ ^[0-9]+$ ]] && (($2 >= 12)) && num "$1" '>=' "$(awk -v n="$2" -v f="$3" 'BEGIN { print n * f }')"; }
follow=$(field .follow "$result")
bpm_ok=$(field .bpmOk "$result")
bar_ok=$(field .barOk "$result")
row "89 BPM follows the on-air player (sim)" "djlink status + clock, 10 s after each change" \
    "$bpm_ok/$follow samples within 0.05 BPM" ">= 95 % of >= 12" "$(ratio_ok "$bpm_ok" "$follow" 0.95 && echo PASS || echo FAIL)"
row "89 bar follows the on-air player (sim)" "djlink status + clock, 10 s after each change" \
    "$bar_ok/$follow within 0.15 beats, max $(field .barMax "$result")" ">= 90 % of >= 12" \
    "$(ratio_ok "$bar_ok" "$follow" 0.9 && echo PASS || echo FAIL)"
bend=$(field .bendMax "$result")
row "89 handover bends without a jump (sim)" "clock beat rate vs on-air BPM, 12 s after a handover" \
    "$(field .handovers "$result") handovers, $(field .bendSamples "$result") samples, max bend $bend" \
    "one handover, bend <= 0.27" \
    "$([[ $(field .handovers "$result") == 1 && $(field .bendSamples "$result") != 0 ]] && pass_if "$bend" '<=' 0.27 || echo FAIL)"
held=$(field .heldMax "$result")
run=$(field .runMax "$result")
row "89 tempo held on link loss (sim)" "clock bpm after the link goes quiet" \
    "$(field .lost "$result") samples, last $(field .lastBpm "$result") BPM, off by <= $held, rate off by <= $run" \
    "held within 0.05 BPM, rate within 3 %" \
    "$([[ $(field .lost "$result") != 0 ]] && [[ $(pass_if "$held" '<=' 0.05) == PASS ]] && pass_if "$run" '<=' 0.03 || echo FAIL)"

# Without sudo: the engine's own UDP sockets, bytes in and out since each opened (nettop reads the user's processes).
# Its receive sockets on 50000-50002 must have sent nothing, and no socket may send to those ports.
if ((DRY)); then
    say "[dry-run] nettop -L 1 -m udp -n -x -p ENGINE -J bytes_in,bytes_out"
    sockets=$([[ -n $SAMPLES && -f $SAMPLES/nettop-udp.csv ]] && cat "$SAMPLES/nettop-udp.csv")
else
    engine=$(pgrep -x nocturne | head -n 1)
    sockets=$([[ -n $engine ]] && nettop -L 1 -m udp -n -x -p "$engine" -J bytes_in,bytes_out 2> /dev/null)
fi
printf '%s\n' "$sockets" > "$OUT/nettop-udp.csv"
read -r n heard sent <<< "$(awk -F, '
  $1 ~ /<->/ {
    split($1, ends, "<->"); local = ends[1]; sub(/.*:/, "", local); remote = ends[2]; sub(/.*:/, "", remote)
    if (local >= 50000 && local <= 50002) { n++; heard += $2; sent += $3 }
    else if (remote >= 50000 && remote <= 50002) sent += $3
  }
  END { printf "%d %d %d\n", n, heard, sent }' "$OUT/nettop-udp.csv")"
row "89 receive only (nettop)" "nettop -m udp -p ENGINE: sockets on or to UDP 50000-50002" \
    "$n sockets, $heard bytes in, $sent bytes out" "3 sockets, bytes in > 0, 0 bytes out" \
    "$( ((n == 3 && heard > 0 && sent == 0)) && echo PASS || echo FAIL)"

if [[ -n $dump ]]; then
    sudo kill "$dump" 2> /dev/null
    wait "$dump" 2> /dev/null
    dump=
    sent=$(sudo tcpdump -n -r "$pcap" "src host $ip" 2> /dev/null | wc -l | tr -d ' ')
    heard=$(sudo tcpdump -n -r "$pcap" "not src host $ip" 2> /dev/null | wc -l | tr -d ' ')
    row "89 receive only (tcpdump)" "sudo tcpdump -i $DJ_IF udp portrange 50000-50002" \
        "$sent sent by the Mini, $heard heard" "0 sent, simulator heard" \
        "$([[ $sent == 0 && ${heard:-0} -gt 0 ]] && echo PASS || echo FAIL)"
elif ((DRY)); then
    row "89 receive only (tcpdump)" "sudo tcpdump -i $DJ_IF udp portrange 50000-50002" "-" "0 sent, simulator heard" SKIP
else
    row "89 receive only (tcpdump)" "sudo tcpdump -i $DJ_IF udp portrange 50000-50002" "no sudo (nettop row above)" \
        "0 sent" SKIP
fi
manual "89 real CDJs" "djlink on en7 (the DJ interface); djlink status during a transition" \
    "BPM, beat in bar and on-air player match the decks; the beat clock follows the on-air CDJ; no CDJ shows an extra device"
