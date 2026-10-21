#!/bin/bash
# The show as it plays (34 cuts, 72), one column switch for the in-transition sample, a bypassed effect's cost (34 cut 1
# and the no-op skips) and Classic Shift Glitch on a playing layer (46 phase 11). Edits are reverted: the previous
# column plays again, the bypass goes back to its value, the added effect is removed.
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"
parse_args "$@"
say "== show performance"

LAYERS='[.composition | .. | objects | select(has("blend") and .playing != null)]'

# --- the show as it plays
s=$(summarize "$(measure show "$AB_SECONDS")")
echo "$s" > "$OUT/summary-show.json"
row "34 render fps" "stats.jsonl render.fps" "$(field .fps "$s")" ">= 59.5" "$(pass_if "$(field .fps "$s")" '>=' 59.5)"
row "34 render GPU steady" "stats.jsonl render.gpuMs, sizes.fading 0" "$(field .gpuSteadyMs "$s") ms" \
    "< 12 (gig: 12-16 at level 1)" "$(pass_if "$(field .gpuSteadyMs "$s")" '<' 12)"
row "34 frame guard level" "stats.jsonl render.budgetLevel" "max $(field .budgetMax "$s")" "0" \
    "$(pass_if "$(field .budgetMax "$s")" '<=' 0)"
row "34 passes" "stats.jsonl render.passes" "max $(field .passesMax "$s")" "reference" INFO
row "engine CPU" "stats.jsonl cpu" "$(field .cpu "$s") %" "reference" INFO
n=$(jq '.outputs | length' <<< "$s")
for i in $(seq 0 $((n - 1))); do
    o=$(jq -c ".outputs[$i]" <<< "$s")
    d=$(field .display "$o")
    row "72 output $d new frames" "stats.jsonl outputs[].new" "$(field .new "$o")/s (min $(field .newMin "$o")), gpu $(field .gpuMs "$o") ms" \
        ">= 59.5" "$(pass_if "$(field .new "$o")" '>=' 59.5)"
    row "72 output $d hitches" "stats.jsonl outputs[].hitches" "$(field .hitchesPerS "$o")/s" "<= 0.05/s" \
        "$(pass_if "$(field .hitchesPerS "$o")" '<=' 0.05)"
done
((n > 0)) || row "72 outputs" "stats.jsonl outputs[]" "none attached" "projector and TV attached" FAIL

snap=$(snapshot)

# --- in a transition: one column switch and back, each followed by TRANSITION_WAIT seconds
TRANSITION_WAIT=${TRANSITION_WAIT:-8}
prev=$(jq -r "(.composition.column // 0) as \$c | if \$c > 0 then \$c else ([$LAYERS[] | .playing.column] | first // empty) end" \
    <<< "$snap" 2> /dev/null)
other=$(jq -r --argjson c "${prev:-0}" '.composition as $k | [$k.decks[]? | select(.id == $k.deck or any(.columns[]; .id == $c))]
    | first // empty | [.columns[].id] as $ids | ($ids | index($c)) as $i
    | [$ids[] | select(. != $c)] as $rest | if ($rest | length) == 0 then empty
      elif $i == null then $rest[0] else $ids[($i + 1) % ($ids | length)] end' <<< "$snap" 2> /dev/null)
if [[ -n $prev && -n $other ]]; then
    from=$(now)
    ctl column "$other" > /dev/null
    pause "$TRANSITION_WAIT"
    ctl column "$prev" > /dev/null
    pause "$TRANSITION_WAIT"
    t=$(summarize "$(measure_since transition "$from")")
    row "34 render GPU in transition" "nocturne-ctl column $other; column $prev; sizes.fading > 0" \
        "$(field .gpuFadeMs "$t") ms" "< 16.7" "$(pass_if "$(field .gpuFadeMs "$t")" '<' 16.7)"
else
    row "34 render GPU in transition" "nocturne-ctl column" "no column to switch to" "a deck with two columns" SKIP
fi

# --- a bypassed effect costs nothing (34 cut 1)
pick=$(jq -c "$LAYERS | map(.id as \$l | .effects[]? | select(.bypass != true) | {layer: \$l, id, type}) | first // empty" \
    <<< "$snap" 2> /dev/null)
if [[ -n $pick ]]; then
    fx=$(field .id "$pick")
    on=$(summarize "$(measure bypass-off 20)")
    ctl fxbypass "$fx" on > /dev/null
    pause 3
    off=$(summarize "$(measure bypass-on 20)")
    ctl fxbypass "$fx" off > /dev/null
    a=$(field .gpuMs "$on") b=$(field .gpuMs "$off")
    row "34/1 bypassed $(field .type "$pick") #$fx (layer $(field .layer "$pick"))" "nocturne-ctl fxbypass $fx on|off" \
        "$a -> $b ms" "bypassed <= active + 0.1" "$(pass_if "$b" '<=' "$(awk -v a="$a" 'BEGIN{print a + 0.1}')")"
else
    row "34/1 bypassed effect" "nocturne-ctl fxbypass" "no effect on a playing layer" "an effect to bypass" SKIP
fi

# --- Classic Shift Glitch on a playing layer (46 phase 11): alternating windows without and with it, steady seconds
# only, the median of the paired differences
GLITCH_ROUNDS=${GLITCH_ROUNDS:-3}
GLITCH_SECONDS=${GLITCH_SECONDS:-10}
GLITCH_SETTLE=${GLITCH_SETTLE:-3}

layer=$(jq -r "$LAYERS | map(.id) | first // empty" <<< "$snap" 2> /dev/null)
if [[ -n $layer ]]; then
    ids() { jq -c "[$LAYERS[] | select(.id == $layer) | .effects[]? | select(.type == \"classicShiftGlitch\") | .id]" <<< "$1"; }
    before=$(ids "$snap")
    pairs=() diffs=() fps=() lost=0
    for r in $(seq 1 "$GLITCH_ROUNDS"); do
        pause "$GLITCH_SETTLE"
        a=$(steady_window "shift-glitch-off-$r" "$GLITCH_SECONDS")
        ctl fx "$layer" classicShiftGlitch > /dev/null
        added=$(jq -r --argjson b "$before" '. - $b | first // empty' <<< "$(ids "$(snapshot)")" 2> /dev/null)
        pause "$GLITCH_SETTLE"
        b=$(steady_window "shift-glitch-on-$r" "$GLITCH_SECONDS")
        if [[ -n $added ]]; then
            ctl fxrm "$added" > /dev/null
        elif ((!DRY)); then
            lost=1
            say "WARNING: could not find the added Shift Glitch on layer $layer; remove it by hand (Cmd-Z in the UI)"
            break
        fi
        off=$(field .gpuMs "$a") on=$(field .gpuMs "$b")
        pairs+=("$off/$on")
        [[ $off != - && $on != - ]] && diffs+=("$(awk -v a="$off" -v b="$on" 'BEGIN { printf "%+.2f", b - a }')")
        fps+=("$(field .fps "$b")")
    done
    median=$(median "${diffs[@]}")
    fpsMin=$(printf '%s\n' "${fps[@]}" | sort -g | head -n 1)
    result=$(pass_if "$median" '<=' 1.2)
    ((lost)) && result=FAIL
    row "46/11 Shift Glitch on layer $layer" \
        "nocturne-ctl fx $layer classicShiftGlitch; fxrm; $GLITCH_ROUNDS rounds of ${GLITCH_SECONDS} s off/on, sizes.fading 0" \
        "median ${median:--} ms GPU (off/on ${pairs[*]:-}; paired ${diffs[*]:-}), ${fpsMin:--} fps" \
        "<= +1.2 ms, 60 fps" "$result"
else
    row "46/11 Shift Glitch" "nocturne-ctl fx" "no playing layer" "a playing layer" SKIP
fi

manual "34 picture unchanged" "look at the projector and the TV" \
    "Classic Stripper strips, edges and the L5 text look as at the gig (cuts 2-7)"
manual "66 generator cost" "a generator per layer, Noise first; read stats.jsonl render.gpuMs" \
    "Noise about 0.15 ms, the others 0.03-0.05 ms"
