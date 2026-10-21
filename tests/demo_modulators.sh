#!/bin/bash
# On-screen demo of modulators and macros on a freshly started engine at 120 BPM (a beat is 0.5 s, a bar 2 s).
# Layer 1 plays a counting clip through Transform, Colorize and Brightness/Contrast; layer 2 is the test card shrunk
# into the top right. Beat LFOs drive one parameter each, then an Energy macro drives several through their own curves,
# first by hand, then from LFOs combined (add and multiply). It ends with audio routes in place (kick into Energy, bass into
# rotation, hats into layer 2's opacity) over a slow idle LFO, so the picture reacts as soon as an audio input
# delivers, and saves them as the Macro Set "f1 pulsing". Screens and slices are left as they are.
#
# usage: demo_modulators.sh CTL MEDIA_DIR [PORT [HOST]]
#   CTL        path to nocturne-ctl
#   MEDIA_DIR  absolute path of a folder on the engine's machine with count_709_tv.mp4
#   PORT       engine port (default 7400), HOST engine host (default 127.0.0.1)
#
# Watch the engine's stats line: no compiled pipelines while modulators run; nothing recompiles.
set -eu

CTL=$1
MEDIA=$2
PORT=${3:-7400}
HOST=${4:-127.0.0.1}

run() {
    "$CTL" --host "$HOST" --port "$PORT" "$@"
}

ctl() {
    run "$@" > /dev/null || { echo "failed: $*" >&2; exit 1; }
}

# Runs an adding command and prints the id of what it added (KEY is macro, modulator or effect).
add() {
    local key=$1
    shift
    run "$@" | tail -1 | sed -E "s/.*\"$key\":\\{[^}]*\"id\":([0-9]+).*/\\1/"
}

# Prints the macros' and modulators' live values once a second for SECONDS.
watch() {
    for _ in $(seq "$1"); do
        sleep 1
        run mod state | tail -1 |
            sed -E 's/.*"macros":(\[[^]]*\]).*"modulators":(\[[^]]*\]).*/macros \1  modulators \2/'
    done
}

step() {
    echo "== $*"
}

step "layer 1: a counting clip through Transform, Colorize and Brightness/Contrast; layer 2: the test card, top right; 120 BPM"
ctl bpm 120
ctl play 1 "$MEDIA/count_709_tv.mp4"
T=$(add effect fx 1 transform scale=0.8)
C=$(add effect fx 1 colorize hue=0 saturation=1 mix=0)
B=$(add effect fx 1 classicBrightnessContrast brightness=0)
ctl addlayer
ctl fx 2 transform scale=0.35 positionX=0.3 positionY=-0.3

step "sine over a bar: Transform scale 0.8 + 0..0.4, largest on the downbeat"
SINE=$(add modulator mod lfo "1.$T.scale" sine 4 0 0.4)
watch 8

step "square per beat: layer 2 shows on the first half of every beat, hidden on the second"
SQUARE=$(add modulator mod lfo 2.opacity square 1 0 -1)
watch 8

step "saw per beat: brightness jumps to 0.6 on every beat and falls off, like a kick"
SAW=$(add modulator mod lfo "1.$B.brightness" saw 1 0 0.6)
watch 8

step "ramp over 4 bars: Colorize hue sweeps the colour wheel at 60% mix"
ctl param 1 "$C" mix 0.6
RAMP=$(add modulator mod lfo "1.$C.hue" ramp 16)
watch 8

step "the saw eased in (easeIn curve): a sharper pump"
ctl mod set "$SAW" curve=easeIn
watch 6

for id in "$SINE" "$SQUARE" "$SAW" "$RAMP"; do
    ctl mod rm "$id"
done
ctl param 1 "$C" mix 0

step "Energy macro: brightness 0..0.6 (easeIn), Transform scale +0..0.5 (easeOut), Colorize mix 0..1; swept by hand"
M=$(add macro macro add Energy)
ctl macro target "$M" "1.$B.brightness" 0 0.6 easeIn
ctl macro target "$M" "1.$T.scale" 0 0.5 easeOut
ctl macro target "$M" "1.$C.mix" 0 1
for v in 0 0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8 0.9 1 0.9 0.8 0.7 0.6 0.5 0.4 0.3 0.2 0.1 0; do
    ctl macro set "$M" "$v"
    sleep 0.3
done
watch 2

step "combined: a sine over 2 bars adds 0..1 to Energy, a square per beat multiplies it by 1 then 0.3"
SLOW=$(add modulator mod lfo "m:$M" sine 8)
GATE=$(add modulator mod lfo "m:$M" square 1 0.3 1 combine=multiply)
watch 12
ctl mod rm "$GATE"
ctl mod rm "$SLOW"

step "audio routes: kick pumps Energy, bass turns the picture up to 10 degrees, hats flash layer 2 (base 0.3)"
step "an idle sine over 4 bars keeps Energy moving at 0..0.25 until audio arrives"
ctl mod lfo "m:$M" sine 16 0 0.25
ctl mod audio "m:$M" kick
ctl mod audio "1.$T.rotation" bass 0 10
ctl opacity 2 0.3
ctl mod audio 2.opacity hat 0 0.7
watch 10

step "saved as the Macro Set \"f1 pulsing\" and loaded back (one undo step)"
run macroset save "f1 pulsing" | tail -1
run macroset load "f1 pulsing" | grep '"op":"load"' || true
watch 3

step "audio input (connect the Focusrite and select it with: $CTL --port $PORT audio select Focusrite)"
run audio input | tail -1
echo "done; the audio routes stay in place"
