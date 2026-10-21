#!/bin/bash
# On-screen demo of text clips, about 18 s on a freshly started engine at 120 BPM (a bar is 2 s). Layer 1 plays a
# counting clip; layer 2, on top, holds two text clips. The text is edited live (content, colour, size, alignment,
# outline, a missing font), its size pulses with a beat LFO, and Autopilot steps between the two text clips with an
# alpha transition, then from text to an empty cell and back.
#
# usage: demo_text.sh CTL MEDIA_DIR [PORT [HOST]]
#   CTL        path to nocturne-ctl
#   MEDIA_DIR  absolute path of a folder on the engine's machine with count_709_tv.mp4
#   PORT       engine port (default 7400), HOST engine host (default 127.0.0.1)
#
# Watch the engine's stats line: 0 video misses, and no compiled pipelines after the first text clip arrives. The log
# warns once that "No Such Font" is not installed.
set -eu

CTL=$1
MEDIA=$2
PORT=${3:-7400}
HOST=${4:-127.0.0.1}

ctl() {
    "$CTL" --host "$HOST" --port "$PORT" "$@" > /dev/null || { echo "failed: $*" >&2; exit 1; }
}

step() {
    echo "== $*"
}

step "a counting clip with a title over it"
ctl bpm 120
ctl quantise off
ctl addlayer
ctl cell 1 1 "$MEDIA/count_709_tv.mp4"
ctl textcell 2 1 'Nocturne\nText clip' size=0.14 color=#ffd833 outline=0.06 outlinecolor=#99004d
ctl textcell 2 2 'second clip' size=0.1 color=#33ccff align=left x=0.05 y=0.85
ctl column 1
sleep 2

step "live edits: text, colour, alignment"
ctl text 2 1 text='Live edit'
sleep 1
ctl text 2 1 color=1,0.3,0.3
sleep 1
ctl text 2 1 align=right x=0.95 y=0.2 size=0.08
sleep 2

step "undo back to the title"
ctl undo
ctl undo
ctl undo
sleep 2

step "a missing font falls back to the system font"
ctl text 2 1 font='No Such Font'
sleep 2
ctl undo

step "size pulses with a beat LFO"
ctl mod lfo 2.textSize sine 1 0 0.06
sleep 2

step "Autopilot steps between the two text clips every bar, crossfading over 2 beats"
ctl transition 2 alpha 2
ctl autopilot 2 next 1
sleep 4

step "cut, and clear the text layer"
ctl autopilot 2 off
ctl transition 2 cut
ctl cellclear 2 1
ctl cellclear 2 2
sleep 1
echo "done"
