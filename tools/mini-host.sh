#!/bin/bash
# The Mac mini's ssh destination, from one place, for every Mini tool. Sourced, it sets MINI (user@host) and
# MINI_HOST (the host alone, for nocturne-ctl --host). The first that is set wins:
#   1. MINI            one command:  MINI=mini tools/deploy-mini.sh
#   2. NOCTURNE_MINI   the shell profile:  export NOCTURNE_MINI=mini
#   3. the first line of $NOCTURNE_MINI_CONFIG (default ~/.config/nocturne/mini)
#   4. mini.local, the Mini's Bonjour name
# user@host, a host name (e.g. the Mini's Tailscale name) or an address; ssh picks the user for a bare host. MINI=local stays.
# Run on its own it prints the destination and where it came from, and connects to nothing.
MINI_DEFAULT=mini.local

mini_host() {
    local config=${NOCTURNE_MINI_CONFIG:-$HOME/.config/nocturne/mini} line=
    [[ ! -f $config ]] || read -r line < "$config" || true
    line=${line//[[:space:]]/}
    if [[ -n ${MINI:-} ]]; then
        MINI_FROM=MINI
    elif [[ -n ${NOCTURNE_MINI:-} ]]; then
        MINI=$NOCTURNE_MINI MINI_FROM=NOCTURNE_MINI
    elif [[ -n $line ]]; then
        MINI=$line MINI_FROM=$config
    else
        MINI=$MINI_DEFAULT MINI_FROM=default
    fi
    MINI_HOST=${MINI#*@}
}

mini_host

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    echo "$MINI ($MINI_FROM)"
fi
