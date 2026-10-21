#!/bin/bash
# usage: mappings.sh CTL [PORT]  - keystone, two tilted halves, four cube faces on screen 1
CTL="$1"; P="${2:-17400}"
c() { "$CTL" --port "$P" "$@"; }
last() { c snapshot | jq '[.composition.screens[].slices[].id] | max'; }
add() { c addslice 1 0 "$@" >/dev/null; last; }
c corners 1 0.03 0.15 0.30 0.05 0.31 0.95 0.02 0.85 >/dev/null
a=$(add 0 0 0.5 1);   c corners "$a" 0.36 0.12 0.49 0.06 0.49 0.90 0.36 0.84 >/dev/null
b=$(add 0.5 0 0.5 1); c corners "$b" 0.51 0.06 0.64 0.14 0.64 0.86 0.51 0.92 >/dev/null
t=$(add 0 0 0.5 0.5);     c corners "$t" 0.83 0.08 0.98 0.22 0.83 0.36 0.68 0.22 >/dev/null
l=$(add 0 0.5 0.5 0.5);   c corners "$l" 0.68 0.22 0.83 0.36 0.83 0.70 0.68 0.56 >/dev/null
r=$(add 0.5 0 0.5 0.5);   c corners "$r" 0.83 0.36 0.98 0.22 0.98 0.56 0.83 0.70 >/dev/null
d=$(add 0.5 0.5 0.5 0.5); c corners "$d" 0.68 0.60 0.83 0.74 0.83 0.94 0.68 0.80 >/dev/null
c snapshot | jq -c '[.composition.screens[].slices[] | {id, inputRect}]'
