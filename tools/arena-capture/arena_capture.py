#!/usr/bin/env python3
"""Phase 11 reference session (ticket 46) and the MovementType option 2 capture (ticket 101).

Drives the reference app's REST API on a scratch layer it adds and removes, and writes work directories in the layout
tools/classic_parity.py and tests/classic_test.cpp read: WORK/cases.json, WORK/<input>, WORK/ref/NAME.png (static
cases) and WORK/seq/NAME.I/ (timed cases: NNN.png and frames.json). The composition is never saved; a changed tempo is
put back.

  arena_capture.py [--dry-run] [--root DIR] [--from P6] [--only WORK...] [--tempo BPM] [--arena-fps N]
                   [--record-pause] [--list]

  --dry-run       print every request, write nothing, contact nothing
  --root DIR      where the work directories go (default ~/Work/azylume/parity-captures/p11)
  --from P6       phase 6 captures holding card/input.png, uv/uv.png and motion/motion.mp4
  --only WORK     capture only these work directories (names as in --list)
  --tempo BPM     the other tempo for the time base cases (default 90)
  --arena-fps N   the frame rate the reference app shows while capturing, recorded in frames.json
  --record-pause  before each ticket 101 motion case, wait for Enter so the layer can be recorded at the display rate
  --list          print the capture list and exit
Python 3 standard library only; Pillow, when installed, lets static captures skip the logo overlay.
"""
import json
import struct
import sys
import time
import urllib.parse
import urllib.request
import zlib
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS))
import classic_parity as cp  # noqa: E402

API = "http://127.0.0.1:3500/api/v1"
W, H = 1920, 1080
PAUSE = 0.2

STRIPPER_101 = {"number": 0.1, "globalWidth": 0.1, "mode": 0.0, "direction": 0.0, "feather": 0.0, "speed": 0.0}
BOTH_WAYS = {"number": 0.54295532599999996037, "movementType": 0.77319587599999994865, "speed": 0.15807560100000001002,
             "globalWidth": 0.15463917499999998983, "minWidth": 0.1, "maxWidth": 0.4}


# ---- the capture list ------------------------------------------------------------------------------------------------

def bloom_cases(input_name):
    out = []

    def case(name, **ours):
        out.append((name, ours))
    if input_name in ("dots", "lines1", "flat", "lines4"):
        for i, a in enumerate((0.05, 0.1, 0.125, 0.25, 0.5)):
            case(f"amount.{i}", amount=a)
    if input_name in ("dots", "lines1", "flat"):
        for a in (0.1, 0.25):
            for it in range(1, 6):
                case(f"iter.a{a:g}.{it}", amount=a, iterations=float(it))
    if input_name in ("dots", "lines1"):
        for it in (1, 2):
            for i, v in enumerate((0, 0.125, 0.25, 0.5, 0.75, 1)):
                case(f"size.i{it}.{i}", amount=0.1, iterations=float(it), size=v)
            for i, v in enumerate((0, 0.25, 0.5, 0.75, 1)):
                case(f"gaussian.i{it}.{i}", amount=0.1, iterations=float(it), gaussian=v)
    if input_name in ("flat", "ramp"):
        for t in (0.9, 0.6):
            for a in (0.1, 1.0):
                for i, k in enumerate((0, 0.25, 0.5, 0.75, 1)):
                    case(f"knee.t{t:g}.a{a:g}.{i}", threshold=t, amount=a, knee=k)
        for k in (0.0, 0.1):
            for i in range(9):
                case(f"threshold.k{k:g}.{i}", threshold=round(0.1 * (i + 1), 2), knee=k, amount=0.1)
    if input_name == "alphaflat":
        case("alpha.default")
        case("alpha.t0.3", threshold=0.3)
    return out


def plan(tempo):
    """Work directory -> (input file, cases, settings). A case is (name, effect, ours, extras)."""
    work = {}

    def add(name, input_name, cases, **settings):
        work[name] = {"input": input_name, "cases": cases, **settings}

    glitch = [("default", "Shift Glitch", {}, {}),
              ("allOne", "Shift Glitch", {"frequency": 1.0, "vertical": 1.0, "horizontal": 1.0, "size": 1.0,
                                          "blackBG": 1.0}, {})]
    seq = {"sequence": True, "seconds": 4.0, "fps": 20}
    add("glitch-card", "input.png", [(n, e, o, {**seq, "instances": 5}) for n, e, o, _ in glitch])
    add("glitch-uv", "uv.png", [(n, e, o, {**seq, "instances": 1}) for n, e, o, _ in glitch])

    timebase = {"sequence": True, "seconds": 6.0, "fps": 20, "instances": 1}
    add("tempo-card", "input.png",
        [(f"frequency.{i}", "Shift Glitch", {"frequency": f}, timebase) for i, f in enumerate((0.0, 0.5, 0.75))] +
        [(f"frequency.{i}", "Strobe", {"frequency": f}, timebase) for i, f in enumerate((0.5, 0.75, 0.875))],
        tempo=tempo)

    trails = {"sequence": True, "seconds": 8.0, "fps": 5, "instances": 1}
    add("trails-motion", "motion.mp4",
        [("fresh.luma.0.5", "Trails", {"mode": 1.0, "feedback": 0.5}, trails),
         ("fresh.luma.0.9", "Trails", {"mode": 1.0, "feedback": 0.9}, trails),
         ("fresh.feedback.1", "Trails", {"feedback": 1.0}, trails),
         ("fresh.lighten.0.9", "Trails", {"mode": 2.0, "feedback": 0.9}, trails),
         ("fresh.default", "Trails", {}, trails)])

    for name in ("flat", "ramp", "dots", "lines1", "lines4", "alphaflat"):
        add(f"bloom-{name}", f"{name}.png", [(n, "Bloom", o, {}) for n, o in bloom_cases(name)])

    motion = {"sequence": True, "seconds": 5.0, "fps": 0, "instances": 1, "record": True}
    boundary = [round(0.70 + i / 100, 2) for i in range(11)]
    stripper = [(f"mt.{v:.2f}", "Stripper", {**STRIPPER_101, "movementType": v}, {}) for v in boundary]
    stripper += [(f"feather.{i}", "Stripper", {**STRIPPER_101, "feather": v}, {})
                 for i, v in enumerate((0.0, 0.25, 0.5, 0.75, 1.0))]
    stripper += [(f"mt2.speed.{i}", "Stripper", {**STRIPPER_101, "movementType": 1.0, "speed": s}, motion)
                 for i, s in enumerate((0.0, 0.02, 0.158))]
    stripper += [("preset.Both_Ways", "Stripper", BOTH_WAYS, {**motion, "mixer": "Add"})]
    add("stripper-uv", "uv.png", stripper)
    return work


def cases_json(entry):
    spec = {e["name"]: e for e in json.loads(cp.SPEC.read_text())["effects"]}
    out = []
    for name, effect, ours, extras in entry["cases"]:
        e = spec[effect]
        unknown = set(ours) - {p["id"] for p in e["params"]}
        if unknown:
            raise SystemExit(f"{effect}: no such controls {sorted(unknown)}")
        extras = dict(extras)
        mixer = extras.pop("mixer", "Alpha")
        params = dict(ours)
        if mixer != "Alpha":
            params["blendMode"] = 1.0
        out.append({"name": f"{e['classicType']}.{name}", "effect": effect, "rest_name": cp.rest_name(e),
                    "type": e["classicType"], "params": params, "rest": cp.rest_values(e, ours), "mixer": mixer,
                    "opacity": 1.0, "input": entry["input"], "sequence": extras.pop("sequence", False), **extras})
    return {"input": entry["input"], "cases": out}


# ---- inputs (our own pictures, 8-bit RGBA PNG) -----------------------------------------------------------------------

def write_png(path, pixel):
    rows = bytearray()
    for y in range(H):
        rows.append(0)
        rows += pixel(y)
    chunk = lambda kind, data: struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b""))


def rects_png(path, rects, background=(0, 0, 0, 255)):
    """Rectangles (x0, y0, x1, y1, rgba) over a background, later ones on top."""
    def row(y):
        line = bytearray(bytes(background) * W)
        for x0, y0, x1, y1, rgba in rects:
            if y0 <= y < y1:
                line[x0 * 4:x1 * 4] = bytes(rgba) * (x1 - x0)
        return line
    write_png(path, row)


def grey(v, a=255):
    g = round(v * 255)
    return (g, g, g, a)


def lines(width):
    xs, ys = [160 + 400 * k for k in range(5)], [140, 540, 940]
    return [(x, 0, x + width, H, (255,) * 4) for x in xs] + [(0, y, W, y + width, (255,) * 4) for y in ys]


def make_input(name, out):
    if name == "flat.png":
        rects_png(out, [(120, 240, 720, 840, grey(0.5)), (1000, 120, 1300, 420, grey(0.3)),
                        (1500, 120, 1800, 420, grey(0.7)), (1250, 620, 1550, 920, grey(1.0))])
    elif name == "ramp.png":
        ramp = bytearray(b"".join(bytes(grey(x / (W - 1))) for x in range(W)))
        black = bytearray(bytes((0, 0, 0, 255)) * W)
        write_png(out, lambda y: ramp if 64 <= y < H - 64 else black)
    elif name == "dots.png":
        dots = []
        for j, y in enumerate((140, 540, 940)):
            for k, x in enumerate(200 + 400 * i for i in range(5)):
                size = 1 if (j + k) % 2 == 0 else 2
                dots.append((x, y, x + size, y + size, (255,) * 4))
        rects_png(out, dots)
    elif name == "lines1.png":
        rects_png(out, lines(1))
    elif name == "lines4.png":
        rects_png(out, lines(4))
    elif name == "alphaflat.png":
        rects_png(out, [(120, 240, 720, 840, grey(0.5, 128))], background=(0, 0, 0, 0))
    else:
        return False
    return True


P6_SOURCES = {"input.png": "card/input.png", "uv.png": "uv/uv.png", "motion.mp4": "motion/motion.mp4"}


# ---- the REST client, real or dry --------------------------------------------------------------------------------------

class Arena:
    def __init__(self, dry):
        self.dry = dry
        self.fake = FakeArena() if dry else None

    def call(self, method, path, body=None, raw=False):
        if "save" in path.lower():
            raise RuntimeError("refusing a request that could save the composition: " + path)
        if self.dry:
            print(f"{method:6} {API}{path}" + (f"  {json.dumps(body)}" if body is not None else ""))
            return self.fake.answer(method, path, body)
        data = body.encode() if isinstance(body, str) else (json.dumps(body).encode() if body is not None else None)
        kind = "text/plain" if isinstance(body, str) else "application/json"
        req = urllib.request.Request(API + path, method=method, data=data,
                                     headers={"Content-Type": kind, "Accept": "*/*"})
        with urllib.request.urlopen(req, timeout=20) as f:
            payload = f.read()
        if raw:
            return payload
        return json.loads(payload) if payload.strip() and method == "GET" else None

    def put(self, param_id, value):
        self.call("PUT", f"/parameter/by-id/{param_id}", {"value": value})

    def sleep(self, seconds):
        if not self.dry:
            time.sleep(seconds)

    def wait_for(self, check, what, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            value = check()
            if value:
                return value
            self.sleep(0.05)
        raise RuntimeError("timed out waiting for " + what)


class FakeArena:
    """Just enough of the reference app's answers for --dry-run to walk the real code path."""

    def __init__(self):
        self.next_id = 1000
        self.layers = [self.layer(), self.layer()]
        self.tempo = {"id": self.new_id(), "value": 120.0}
        self.spec = {cp.rest_name(e): e for e in json.loads(cp.SPEC.read_text())["effects"]}

    def new_id(self):
        self.next_id += 1
        return self.next_id

    def param(self, value=None):
        return {"id": self.new_id(), "value": value}

    def layer(self):
        return {"id": self.new_id(), "video": {"opacity": self.param(1.0), "mixer": {"Blend Mode": self.param("Alpha")},
                                               "effects": [{"name": "Transform", "params": {}}]},
                "clips": [{"connected": {"value": "Empty"}}]}

    def effect(self, rest_name):
        e = self.spec[rest_name]
        names = set(cp.rest_values(e, {})) | {"Opacity"}
        return {"name": rest_name, "params": {n: self.param() for n in sorted(names)},
                "mixer": {"Blend Mode": self.param("Alpha")}, "bypassed": self.param(False)}

    def by_id(self, path):
        lid = int(path.split("/by-id/")[1].split("/")[0])
        return next(l for l in self.layers if l["id"] == lid)

    def answer(self, method, path, body):
        if path == "/product":
            return {"name": "dry run"}
        if path == "/composition":
            return {"layers": self.layers, "tempocontroller": {"tempo": self.tempo}}
        if path == "/composition/layers/add":
            self.layers.append(self.layer())
        elif path == "/composition/monitors":
            return [{"id": 9000 + i, "associated_object": f"/composition/layers/by-id/{l['id']}"}
                    for i, l in enumerate(self.layers)]
        elif path.endswith("/snapshot.png"):
            return b""
        elif path.endswith("/effects/video/add"):
            self.by_id(path)["video"]["effects"].append(self.effect(urllib.parse.unquote(body.rsplit("/", 1)[1])))
        elif "/effects/video/" in path and method == "DELETE":
            self.by_id(path)["video"]["effects"].pop(int(path.rsplit("/", 1)[1]))
        elif "/clips/1/open" in path:
            self.layers[int(path.split("/")[3]) - 1]["clips"][0]["connected"]["value"] = "Disconnected"
        elif "/clips/1/connect" in path:
            self.layers[int(path.split("/")[3]) - 1]["clips"][0]["connected"]["value"] = "Connected"
        elif method == "DELETE" and "/composition/layers/by-id/" in path:
            self.layers.remove(self.by_id(path))
        elif method == "GET" and "/composition/layers/by-id/" in path:
            return self.by_id(path)
        elif path.startswith("/parameter/by-id/") and int(path.rsplit("/", 1)[1]) == self.tempo["id"]:
            self.tempo["value"] = body["value"]
        return None


# ---- the session -------------------------------------------------------------------------------------------------------

class Scratch:
    """A layer of our own at the end of the open composition, removed again on exit, with its monitor."""

    def __init__(self, arena):
        self.a = arena

    def __enter__(self):
        a = self.a
        if not a.call("GET", "/product"):
            raise RuntimeError("the reference app does not answer")
        before = {l["id"] for l in a.call("GET", "/composition")["layers"]}
        a.call("POST", "/composition/layers/add")
        self.id = a.wait_for(lambda: next((l["id"] for l in a.call("GET", "/composition")["layers"]
                                           if l["id"] not in before), None), "the scratch layer")
        video = self.layer()["video"]
        a.put(video["opacity"]["id"], 1.0)
        a.put(video["mixer"]["Blend Mode"]["id"], "Alpha")
        self.monitor = next(m["id"] for m in a.call("GET", "/composition/monitors")
                            if m["associated_object"] == f"/composition/layers/by-id/{self.id}")
        return self

    def __exit__(self, *exc):
        self.a.call("DELETE", f"/composition/layers/by-id/{self.id}")

    def layer(self):
        return self.a.call("GET", f"/composition/layers/by-id/{self.id}")

    def effects(self):
        return self.layer()["video"]["effects"]

    def fresh(self, rest_name):
        """Removes every effect but the built-in Transform and adds a new instance of this one."""
        while len(self.effects()) > 1:
            self.a.call("DELETE", f"/composition/layers/by-id/{self.id}/effects/video/{len(self.effects()) - 1}")
            self.a.sleep(0.05)
        self.a.call("POST", f"/composition/layers/by-id/{self.id}/effects/video/add",
                    "effect:///video/" + urllib.parse.quote(rest_name))
        return self.a.wait_for(lambda: len(self.effects()) > 1 and self.effects()[-1], rest_name)

    def show(self, file):
        index = next(i for i, l in enumerate(self.a.call("GET", "/composition")["layers"], 1) if l["id"] == self.id)
        self.a.call("POST", f"/composition/layers/{index}/clips/1/open",
                    "file://" + urllib.parse.quote(str(Path(file).resolve())))
        self.a.wait_for(lambda: self.layer()["clips"][0]["connected"]["value"] != "Empty", "the clip")
        self.a.call("POST", f"/composition/layers/{index}/clips/1/connect")
        self.a.wait_for(lambda: self.layer()["clips"][0]["connected"]["value"] == "Connected", "the clip to play")

    def snap(self):
        return self.a.call("GET", f"/composition/monitors/{self.monitor}/snapshot.png", raw=True)

    def settled(self):
        """A frame once two in a row agree, skipping logo overlays; (frame, settled)."""
        last = None
        for _ in range(4 if not self.a.dry else 1):
            for _ in range(8 if not self.a.dry else 1):
                self.a.sleep(0.15)
                frame = self.clean()
                if frame == last:
                    return frame, True
                last = frame
            self.a.sleep(2.0)
        return last, self.a.dry

    def clean(self):
        for _ in range(40):
            frame = self.snap()
            if self.a.dry or not overlaid(frame):
                return frame
            self.a.sleep(0.25)
        raise RuntimeError("the reference keeps overlaying its logo")


def overlaid(png):
    try:
        return cp.overlaid(png)
    except ImportError:
        return False


def set_case(arena, effect, case):
    for name, value in case["rest"].items():
        arena.put(effect["params"][name]["id"], value)
    arena.put(effect["params"]["Opacity"]["id"], case["opacity"])
    arena.put(effect["mixer"]["Blend Mode"]["id"], case["mixer"])


def write(arena, path, data):
    if arena.dry:
        print(f"       would write {path}")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data if isinstance(data, bytes) else data.encode())


def capture_work(arena, s, root, name, entry, listing, args):
    work = root / name
    s.show(work / entry["input"])
    bare, _ = s.settled()
    write(arena, work / "ref/input.png", bare)
    tempo = arena.call("GET", "/composition")["tempocontroller"]["tempo"]["value"]
    log = {"tempo": tempo, "arenaFps": args["arena_fps"], "unsettled": []}
    for case in listing["cases"]:
        arena.sleep(PAUSE)
        for i in range(case.get("instances", 1)):
            effect = s.fresh(case["rest_name"])
            if case["sequence"]:
                sequence(arena, s, work, case, effect, i, tempo, args)
                continue
            set_case(arena, effect, case)
            frame, settled = s.settled()
            if not settled:
                log["unsettled"].append(case["name"])
            write(arena, work / "ref" / (case["name"] + ".png"), frame)
    write(arena, work / "capture-log.json", json.dumps(log, indent=1))


def sequence(arena, s, work, case, effect, instance, tempo, args):
    """Bypassed while the values are set, switched on at t = 0; every snapshot records when it was asked for and when
    it arrived. fps 0 takes them as fast as they come."""
    arena.put(effect["bypassed"]["id"], True)
    set_case(arena, effect, case)
    arena.sleep(0.3)
    if case.get("record") and args["record_pause"] and not arena.dry:
        input(f"{case['name']}: start recording the scratch layer, then press Enter (stop it after "
              f"{case['seconds'] + 1:g} s) ")
    out = work / "seq" / f"{case['name']}.{instance}"
    frames, fps = [], case["fps"]
    arena.put(effect["bypassed"]["id"], False)
    t0 = time.monotonic()
    while time.monotonic() - t0 < case["seconds"]:
        asked = time.monotonic() - t0
        png = s.snap()
        came = time.monotonic() - t0
        file = "%03d.png" % len(frames)
        write(arena, out / file, png)
        frames.append({"file": file, "asked": round(asked, 4), "came": round(came, 4)})
        if arena.dry:
            print(f"       ... repeated for {case['seconds']:g} s" + (f" at up to {fps}/s" if fps else " as fast as "
                                                                        "snapshots come"))
            break
        if fps:
            time.sleep(max(0.0, 1.0 / fps - (came - asked)))
    write(arena, out / "frames.json", json.dumps(
        {"case": case["name"], "instance": instance, "params": case["params"], "rest": case["rest"],
         "input": case["input"], "tempo": tempo, "arenaFps": args["arena_fps"], "seconds": case["seconds"],
         "marker": "t = 0 when the effect leaves bypass, its values already set", "frames": frames}, indent=1))


def prepare(arena, root, p6, name, entry, listing):
    work = root / name
    target = work / entry["input"]
    if arena.dry:
        print(f"       would write {work / 'cases.json'} ({len(listing['cases'])} cases) and {target}")
        return
    work.mkdir(parents=True, exist_ok=True)
    (work / "cases.json").write_text(json.dumps(listing, indent=1))
    if target.exists() or make_input(entry["input"], target):
        return
    source = p6 / P6_SOURCES[entry["input"]]
    if not source.exists():
        raise SystemExit(f"missing {source}: pass --from with the phase 6 captures")
    target.write_bytes(source.read_bytes())


def with_tempo(arena, bpm, run):
    tempo = arena.call("GET", "/composition")["tempocontroller"]["tempo"]
    old = tempo["value"]
    arena.put(tempo["id"], float(bpm))
    try:
        run()
    finally:
        arena.put(tempo["id"], old)


def print_list(work):
    for name, entry in work.items():
        listing = cases_json(entry)
        print(f"{name}  ({entry['input']}{', tempo %g' % entry['tempo'] if 'tempo' in entry else ''})")
        for c in listing["cases"]:
            how = (f"seq {c['seconds']:g} s x{c['instances']} @{c['fps'] or 'max'}/s" if c["sequence"]
                   else "still")
            print(f"  {c['name']:44} {how:22} {c['mixer']:5} {json.dumps(c['rest'])}")


def main(argv):
    def option(flag, default):
        return argv[argv.index(flag) + 1] if flag in argv else default
    root = Path(option("--root", "~/Work/azylume/parity-captures/p11")).expanduser()
    p6 = Path(option("--from", "~/Work/azylume/parity-captures/p6")).expanduser()
    tempo = float(option("--tempo", 90))
    args = {"arena_fps": float(option("--arena-fps", 0)) or None, "record_pause": "--record-pause" in argv}
    only = [argv[i + 1] for i, a in enumerate(argv) if a == "--only"]
    work = plan(tempo)
    if "--list" in argv:
        print_list(work)
        return 0
    unknown = set(only) - set(work)
    if unknown:
        raise SystemExit(f"no such work directory: {sorted(unknown)}")
    arena = Arena("--dry-run" in argv)
    chosen = {n: e for n, e in work.items() if not only or n in only}
    listings = {n: cases_json(e) for n, e in chosen.items()}
    for name, entry in chosen.items():
        prepare(arena, root, p6, name, entry, listings[name])
    with Scratch(arena) as s:
        for name, entry in chosen.items():
            print(f"== {name}", flush=True)
            run = lambda: capture_work(arena, s, root, name, entry, listings[name], args)
            if "tempo" in entry:
                with_tempo(arena, entry["tempo"], run)
            else:
                run()
    print("done:", ", ".join(chosen))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
