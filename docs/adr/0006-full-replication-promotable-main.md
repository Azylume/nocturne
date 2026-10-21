# Every node holds the full composition and all media

Superseded by 0011.

Each node replicates the whole composition and every media file, not just what its own screens need. Media is synced before the show, at home or at the venue after automatic discovery, so the network carries only small edits and clock messages during the show.

The cost is copying everything to every machine. In return:
- any node can later take over as main engine when the machine running the UI dies, and the UI reconnects to it;
- several UIs (e.g. a phone) can join.

v1 only keeps rendering from the last state; takeover comes later, but the protocol is designed for it from the start.
