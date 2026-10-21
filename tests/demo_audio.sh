#!/bin/bash
# Live demo of the audio input and analysis on a running engine: lists the input devices, picks DEVICE (falling back
# to the default input while it is missing), shows the live meters, then shortens the kick release and adds latency
# so the change is visible, and restores the defaults.
#
# usage: demo_audio.sh CTL [DEVICE [PORT [HOST]]]
#   CTL     path to nocturne-ctl
#   DEVICE  input UID, name or part of a name (default "default", the system's default input)
#   PORT    engine port (default 7400), HOST engine host (default 127.0.0.1)
#
# Clap or play music near the input: kick, snare and hat counts rise, the meters move. "digital silence" means the
# engine binary has no microphone permission (see README, nocturne --mic-check).
set -eu

CTL=$1
DEVICE=${2:-default}
PORT=${3:-7400}
HOST=${4:-127.0.0.1}

ctl() {
    "$CTL" --host "$HOST" --port "$PORT" "$@"
}

# Shows the live meters for SECONDS.
watch() {
    ctl audio watch 20 &
    local pid=$!
    sleep "$1"
    kill "$pid" 2> /dev/null || true
    wait "$pid" 2> /dev/null || true
    echo
}

echo "== devices"
ctl audio devices
echo "== select $DEVICE"
ctl audio select "$DEVICE"
echo "== live analysis, 15 s"
watch 15
echo "== kick release 40 ms (short flashes), 15 s"
ctl audio envelope kick 0 40 > /dev/null
watch 15
echo "== kick release 600 ms and 200 ms latency, 15 s"
ctl audio envelope kick 0 600 > /dev/null
ctl audio latency 200 > /dev/null
watch 15
ctl audio envelope kick 0 150 > /dev/null
ctl audio latency 0 > /dev/null
echo "== input"
ctl audio input
