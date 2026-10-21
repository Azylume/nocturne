#!/bin/bash
# usage: mini-lock.sh acquire|release|status [owner]
set -euo pipefail
. "$(dirname "$0")/mini-host.sh"
HOST=$MINI
LOCK='$HOME/azl.lock'
cmd="${1:?acquire|release|status}"; owner="${2:-$(whoami)@$(hostname -s)}"
case "$cmd" in
  acquire)
    ssh "$HOST" "mkdir $LOCK 2>/dev/null && echo '$owner' > $LOCK/owner && date +%FT%T >> $LOCK/owner" \
      || { echo "Mini busy: $(ssh "$HOST" "cat $LOCK/owner 2>/dev/null | tr '\n' ' '")" >&2; exit 1; } ;;
  release) ssh "$HOST" "command rm -rf $LOCK" ;;
  status) ssh "$HOST" "cat $LOCK/owner 2>/dev/null || echo free" ;;
  *) echo "unknown command: $cmd" >&2; exit 2 ;;
esac
