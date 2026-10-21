#!/bin/bash
# usage: load.sh start|stop|status
#        load.sh run -- command...   (starts the load, runs the command, always stops the load)
# Artificial CPU load for reproducing flaky tests: 16 busy loops at normal priority, more than the M2 Pro's 12 cores,
# the way other agents' builds and a browser load the laptop while the tests run at nice 15 (8 loops leave cores idle
# and reproduce nothing). Each spinner also ends by itself after
# AZL_LOAD_MAX_SECONDS (default 600), so a forgotten or killed session never leaves the machine loaded.
set -euo pipefail
SPINNERS=16
PIDS="${TMPDIR:-/tmp}/azl-load.pids"
MAX_SECONDS="${AZL_LOAD_MAX_SECONDS:-600}"

start() {
  [ "$(ps -o nice= -p $$ | tr -d ' ')" = 0 ] || echo "load: warning, the spinners inherit this shell's nice value" >&2
  [ -s "$PIDS" ] && { echo "load already running ($(wc -l < "$PIDS" | tr -d ' ') spinners)" >&2; return 1; }
  for _ in $(seq "$SPINNERS"); do
    perl -e 'alarm shift; 1 while 1' "$MAX_SECONDS" > /dev/null 2>&1 &
    echo $! >> "$PIDS"
  done
  echo "load: $SPINNERS spinners for at most ${MAX_SECONDS}s"
}

stop() {
  [ -f "$PIDS" ] || return 0
  while read -r pid; do kill "$pid" 2>/dev/null || true; done < "$PIDS"
  command rm -f "$PIDS"
  echo "load: stopped"
}

case "${1:?start|stop|status|run}" in
  start) start ;;
  stop) stop ;;
  status) [ -s "$PIDS" ] && echo "running: $(tr '\n' ' ' < "$PIDS")" || echo "stopped" ;;
  run)
    shift; [ "${1:-}" = "--" ] && shift
    trap stop EXIT INT TERM
    start
    "$@" ;;
  *) echo "unknown command: $1" >&2; exit 2 ;;
esac
