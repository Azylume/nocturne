#!/usr/bin/env python3
"""Plays two CDJs (players 2 and 3) and a DJM (0x21) on Pro DJ Link, to test the DJ link follower without hardware.

Sends keep-alives on UDP 50000, beats and the mixer's channels-on-air on 50001, and player and mixer status on 50002, as
a scripted scenario (built in, or --scenario FILE). Never run it on a network with real CDJs: it claims their numbers.

  djlink_sim.py --broadcast 192.168.1.255              the built-in scenario, about 90 s
  djlink_sim.py --broadcast 192.168.1.255 --bpm 124    the same at another tempo
  djlink_sim.py --dry-run                              every packet, on a virtual clock, nothing sent

Scenario file (JSON): {"players": {"2": {"bpm": 128, "playing": true}, "3": {"bpm": 127.5, "trackBpm": 127.5,
"grid": true}}, "onAir": [2], "master": 2, "events": [{"at": SECONDS, ...}]} where an event is one of
  {"player": 3, "play": true, "phaseFrom": 2, "offsetBeats": 2}   start (or stop with false); its bar OFFSET beats
                                                                  ahead of player 2's
  {"onAir": [2, 3]}  {"master": 3}                                the mixer's on-air channels, the tempo master
  {"player": 3, "rampTo": 130, "seconds": 16}                     bend the tempo with the pitch
  {"loss": true}  {"loss": false}                                 the link goes quiet, or comes back
  {"end": true}
"""
import argparse
import json
import math
import socket
import sys
import time

sys.dont_write_bytecode = True

from djlink_packets import CDJ, DJM, PITCH_NORMAL, beat, cdj_status, keep_alive, mixer_status, on_air  # noqa: E402

MIXER = 0x21
KEEP_ALIVE_EVERY = 1.5
STATUS_EVERY = 0.2
ON_AIR_EVERY = 0.3
MAX_STEP = 0.05
PLAYING, CUED = 0x03, 0x06


def builtin(bpm):
    return {
        "players": {"2": {"bpm": bpm, "playing": True}, "3": {"bpm": bpm - 0.5}},
        "onAir": [2],
        "master": 2,
        "events": [
            {"at": 20, "player": 3, "play": True, "phaseFrom": 2, "offsetBeats": 2},
            {"at": 30, "onAir": [2, 3]},
            {"at": 40, "onAir": [3], "master": 3},
            {"at": 45, "player": 2, "play": False},
            {"at": 50, "player": 3, "rampTo": bpm + 2, "seconds": 16},
            {"at": 75, "loss": True},
            {"at": 90, "end": True},
        ],
    }


class Player:
    def __init__(self, number, spec):
        self.number = number
        self.bpm = float(spec["bpm"])
        self.track_bpm = float(spec.get("trackBpm", self.bpm))
        self.grid = spec.get("grid", True)
        self.playing = spec.get("playing", False)
        # Beats since the start of the track; whole beats are beats, beat 0 a downbeat.
        self.position = 0.0
        self.next_beat = 1
        self.ramp = None

    def tempo(self, t):
        if self.ramp:
            t0, t1, start, end = self.ramp
            if t >= t1:
                self.bpm, self.ramp = end, None
            else:
                self.bpm = start + (end - start) * (t - t0) / (t1 - t0)
        return self.bpm

    def pitch(self):
        return round(PITCH_NORMAL * self.bpm / self.track_bpm)

    def in_bar(self):
        return int(math.floor(self.position + 1e-9)) % 4 + 1

    def until_beat(self, t):
        return (self.next_beat - self.position) * 60.0 / self.tempo(t)


class Sim:
    def __init__(self, scenario, send, wait, log):
        self.players = {int(n): Player(int(n), s) for n, s in scenario["players"].items()}
        self.on_air = set(scenario.get("onAir", []))
        self.master = scenario.get("master", 0)
        self.events = sorted(scenario.get("events", []), key=lambda e: e["at"])
        self.send, self.wait, self.log = send, wait, log
        self.lost = False

    def emit(self, t, port, data, summary):
        if not self.lost:
            self.send(t, port, data, summary)

    def event(self, t, e):
        self.log(t, json.dumps({k: v for k, v in e.items() if k != "at"}))
        if "loss" in e:
            self.lost = e["loss"]
        if "onAir" in e:
            self.on_air = set(e["onAir"])
        if "master" in e:
            self.master = e["master"]
        p = self.players.get(e.get("player"))
        if p and "play" in e:
            p.playing = e["play"]
            source = self.players.get(e.get("phaseFrom"))
            if source:
                p.position = source.position + e.get("offsetBeats", 0)
                p.next_beat = math.floor(p.position) + 1
        if p and "rampTo" in e:
            p.ramp = (t, t + max(e.get("seconds", 0), 1e-3), p.tempo(t), float(e["rampTo"]))
        return not e.get("end")

    def keep_alives(self, t):
        for n in sorted(self.players) + [MIXER]:
            mixer = n == MIXER
            data = keep_alive(n, DJM if mixer else CDJ, [0x02, 0, 0, 0, 0, n], [169, 254, 10, n], 2 if mixer else 1)
            self.emit(t, 0, data, f"keep-alive {n:#x}" if mixer else f"keep-alive {n}")

    def statuses(self, t):
        for n, p in sorted(self.players.items()):
            flags = (0x40 if p.playing else 0) | (0x20 if n == self.master else 0) | (0x08 if n in self.on_air else 0)
            count = math.floor(p.position) + 1 if p.grid else None
            data = cdj_status(n, 1000 + n, p.track_bpm if p.grid else None, p.pitch(), count, p.in_bar(),
                              PLAYING if p.playing else CUED, flags)
            self.emit(t, 2, data, f"status {n} {'playing' if p.playing else 'cued'} {p.bpm:.2f} bpm"
                      f"{' master' if n == self.master else ''}{' on-air' if n in self.on_air else ''}"
                      f"{'' if p.grid else ' no-grid'}")
        m = self.players.get(self.master)
        if m:
            self.emit(t, 2, mixer_status(m.bpm, m.in_bar(), False), f"mixer status {m.bpm:.2f} bpm {m.in_bar()}")

    def channels(self, t):
        flags = [1 if c in self.on_air else 0 for c in range(1, 5)]
        self.emit(t, 1, on_air(flags), f"on-air {flags}")

    def beat(self, t, p):
        p.position = float(p.next_beat)
        p.next_beat += 1
        if p.grid:
            bpm = p.tempo(t)
            self.emit(t, 1, beat(p.number, p.track_bpm, p.pitch(), p.in_bar(), interval_ms=round(60000 / bpm)),
                      f"beat {p.number} {p.in_bar()}/4 {bpm:.2f} bpm")

    def run(self):
        t = 0.0
        due = {"keep": 0.0, "status": 0.0, "channels": 0.0}
        while True:
            while self.events and self.events[0]["at"] <= t + 1e-9:
                if not self.event(t, self.events.pop(0)):
                    return
            if t >= due["keep"]:
                self.keep_alives(t)
                due["keep"] += KEEP_ALIVE_EVERY
            if t >= due["channels"]:
                self.channels(t)
                due["channels"] += ON_AIR_EVERY
            if t >= due["status"]:
                self.statuses(t)
                due["status"] += STATUS_EVERY
            playing = [p for p in self.players.values() if p.playing]
            waits = [MAX_STEP] + [d - t for d in due.values()] + [p.until_beat(t) for p in playing]
            step = max(min(waits + [e["at"] - t for e in self.events[:1]]), 0.0)
            for p in playing:
                p.position += step * p.tempo(t) / 60.0
            t += step
            self.wait(t)
            for p in playing:
                if p.position >= p.next_beat - 1e-6:
                    self.beat(t, p)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--broadcast", metavar="ADDR", help="where to send (a broadcast address, or 127.0.0.1); required")
    ap.add_argument("--interface", metavar="IF", help="send out of this interface only (e.g. en0)")
    ap.add_argument("--scenario", metavar="FILE", help="a scenario file instead of the built-in one")
    ap.add_argument("--bpm", type=float, default=128.0, help="the built-in scenario's tempo (default 128)")
    ap.add_argument("--ports", default="50000,50001,50002", help="the three ports (tests use free loopback ones)")
    ap.add_argument("--dry-run", action="store_true", help="print every packet on a virtual clock, send nothing")
    ap.add_argument("--quiet", action="store_true", help="no event lines")
    args = ap.parse_args()

    if not args.dry_run and not args.broadcast:
        ap.error("--broadcast ADDR is required (nothing is sent by default)")
    ports = [int(p) for p in args.ports.split(",")]
    if len(ports) != 3:
        ap.error("--ports takes three ports")
    if args.scenario:
        with open(args.scenario) as f:
            scenario = json.load(f)
    else:
        scenario = builtin(args.bpm)

    def log(t, text):
        if not args.quiet:
            print(f"{t:8.3f}  {text}", file=sys.stderr, flush=True)

    if args.dry_run:
        def send(t, port, data, summary):
            print(f"{t:8.3f}  {ports[port]}  {summary}  {data.hex()}")

        Sim(scenario, send, lambda t: None, log).run()
        return

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    if args.interface:
        if sys.platform == "darwin":
            sock.setsockopt(socket.IPPROTO_IP, getattr(socket, "IP_BOUND_IF", 25), socket.if_nametoindex(args.interface))
        else:
            sock.setsockopt(socket.SOL_SOCKET, getattr(socket, "SO_BINDTODEVICE", 25), args.interface.encode())
    start = time.monotonic()

    def send(t, port, data, summary):
        sock.sendto(data, (args.broadcast, ports[port]))

    def wait(t):
        delay = start + t - time.monotonic()
        if delay > 0:
            time.sleep(delay)

    log(0.0, f"sending to {args.broadcast} ports {ports}")
    try:
        Sim(scenario, send, wait, log).run()
    except KeyboardInterrupt:
        pass
    log(time.monotonic() - start, "done")


if __name__ == "__main__":
    main()
