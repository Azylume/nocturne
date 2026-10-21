#!/usr/bin/env python3
"""Parity harness for the Classic effect pack (ticket 46).

Drives the reference app over its local REST API on a scratch layer it adds and removes, captures that layer's
monitor, renders the same cases with our engine and scores them. Reference frames stay in the work directory; only
numbers enter the repo. The reference composition is never saved.

  classic_parity.py discover [--cached] [EFFECT...]  controls and slider response -> the spec's display fields
  classic_parity.py inputs DIR             the probe inputs: uv.png, alpha.png and the marker clip motion.mp4
  classic_parity.py cases WORK EFFECT... [--input FILE]  sweeps (9 points per range, every step of a range with
                                           fewer, every choice, both booleans, colours at a few hues), bundled presets
  classic_parity.py render WORK            our test card (WORK/input.png), our frames and their scores
  classic_parity.py capture WORK [FILTER]  reference frames of the input and each static case into WORK/ref
  classic_parity.py sequence WORK [FILTER] [--seconds S] [--instances N]  timed reference frames of the time-based
                                           cases into WORK/seq/NAME.I/ with frames.json
  classic_parity.py timeline WORK [FILTER] per frame of each sequence: time, mean colour, share changed, clip frame
  classic_parity.py score WORK [--write]   per control and per effect scores -> docs/research/classic-parity.json
  classic_parity.py instances WORK [N]     N - 1 more reference instances of each Stripper case (random layouts)
  classic_parity.py strips WORK [--write]  Stripper cases scored on layout statistics over the instances
  classic_parity.py trails-phase WORK      the clip phase of each Trails sequence -> WORK/phase.json
  classic_parity.py sequences WORK [ONLY]  our sequences of the timed cases -> WORK/ours/seq/NAME.I.json
  classic_parity.py timed P6 [--write]     Strobe, Shift Glitch and Trails scored from the card, uv and motion work
                                           directories under P6 (after trails-phase and sequences)
Order: cases, render (makes the input), capture, render again (scores), score. With --input the work directory
shows WORK/FILE instead of the test card (cases.json records it); render still draws the test card. Needs Pillow and
numpy.
"""
import json
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

API = "http://127.0.0.1:3500/api/v1"
REPO = Path(__file__).resolve().parent.parent
SPEC = REPO / "docs/research/classic-controls.json"


def call(method, path, body=None, raw=False):
    data = body.encode() if isinstance(body, str) else (json.dumps(body).encode() if body is not None else None)
    kind = "text/plain" if isinstance(body, str) else "application/json"
    req = urllib.request.Request(API + path, method=method, data=data, headers={"Content-Type": kind,
                                                                                "Accept": "*/*"})
    with urllib.request.urlopen(req, timeout=20) as f:
        payload = f.read()
    if raw:
        return payload
    return json.loads(payload) if payload.strip() and method == "GET" else None


def wait_for(check, what, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        value = check()
        if value:
            return value
        time.sleep(0.05)
    raise RuntimeError("timed out waiting for " + what)


class Scratch:
    """A layer of our own at the end of the open composition, removed again on exit."""

    def __enter__(self):
        product = call("GET", "/product")
        if not product:
            raise RuntimeError("the reference app does not answer")
        before = {l["id"] for l in call("GET", "/composition")["layers"]}
        call("POST", "/composition/layers/add")
        self.id = wait_for(lambda: next((l["id"] for l in call("GET", "/composition")["layers"]
                                         if l["id"] not in before), None), "the scratch layer")
        video = self.layer()["video"]
        put_value(video["opacity"]["id"], 1.0)
        put_value(video["mixer"]["Blend Mode"]["id"], "Alpha")
        monitors = call("GET", "/composition/monitors")
        self.monitor = next(m["id"] for m in monitors if m["associated_object"] == f"/composition/layers/by-id/{self.id}")
        return self

    def __exit__(self, *exc):
        layers = call("GET", "/composition")["layers"]
        index = next(i for i, l in enumerate(layers, 1) if l["id"] == self.id)
        call("DELETE", f"/composition/layers/{index}")

    def layer(self):
        return call("GET", f"/composition/layers/by-id/{self.id}")

    def effects(self):
        return self.layer()["video"]["effects"]

    def add(self, name):
        count = len(self.effects())
        call("POST", f"/composition/layers/by-id/{self.id}/effects/video/add", "effect:///video/" + urllib.parse.quote(name))
        return wait_for(lambda: len(self.effects()) > count and self.effects()[-1], name)

    def clear_effects(self):
        while len(self.effects()) > 1:
            call("DELETE", f"/composition/layers/by-id/{self.id}/effects/video/{len(self.effects()) - 1}")
            time.sleep(0.05)

    def show(self, png):
        index = next(i for i, l in enumerate(call("GET", "/composition")["layers"], 1) if l["id"] == self.id)
        call("POST", f"/composition/layers/{index}/clips/1/open", "file://" + urllib.parse.quote(str(Path(png).resolve())))
        wait_for(lambda: self.layer()["clips"][0]["connected"]["value"] != "Empty", "the clip")
        call("POST", f"/composition/layers/{index}/clips/1/connect")
        wait_for(lambda: self.layer()["clips"][0]["connected"]["value"] == "Connected", "the clip to play")

    def frame(self):
        """The layer's monitor, skipping the frames the reference overlays with its logo now and then."""
        for _ in range(40):
            frame = call("GET", f"/composition/monitors/{self.monitor}/snapshot.png", raw=True)
            if not overlaid(frame):
                return frame
            time.sleep(0.25)
        raise RuntimeError("the reference keeps overlaying its logo")

    def snapshot(self):
        """A frame of the layer once two in a row agree; None when it keeps changing."""
        last = None
        for _ in range(8):
            time.sleep(0.15)
            frame = self.frame()
            if frame == last:
                return frame
            last = frame
        return None

    def settled(self):
        """A snapshot, waiting out a fading logo overlay a few times; None when the picture keeps changing."""
        for _ in range(4):
            frame = self.snapshot()
            if frame is not None:
                return frame
            time.sleep(2.0)
        return None


def overlaid(png):
    """The logo overlay at full strength: a white logo above the centre over the picture with its alpha changed (222 on
    an opaque picture, the same along each edge)."""
    import io
    from PIL import Image
    image = Image.open(io.BytesIO(png)).convert("RGBA")
    w, h = image.size
    top, bottom = [[image.getpixel((x, y))[3] for x in (2, w - 3)] for y in (2, h - 3)]
    return image.getpixel((w // 2, h * 200 // 1080)) == (255, 255, 255, 255) and \
        top[0] == top[1] < 250 and bottom[0] == bottom[1] < 250


def main():
    command = sys.argv[1] if len(sys.argv) > 1 else ""
    commands = {"discover": discover, "inputs": inputs, "cases": cases, "capture": capture, "render": render,
                "sequence": sequence, "timeline": timeline, "score": score, "instances": instances,
                "strips": strip_stats, "sequences": render_sequences, "trails-phase": trails_phase, "timed": timed}
    if command not in commands:
        print(__doc__)
        return 2
    return commands[command](*sys.argv[2:])


def put_value(param_id, value):
    call("PUT", f"/parameter/by-id/{param_id}", {"value": value})


def get_value(param_id):
    return call("GET", f"/parameter/by-id/{param_id}")["value"]


def describe(param, sweep):
    out = {"type": param["valuetype"]}
    for key in ("min", "max", "options"):
        if key in param:
            out[key] = param[key]
    out["default"] = param.get("value")
    if param.get("view"):
        out["view"] = param["view"]
    if sweep and param["valuetype"] == "ParamRange":
        lo, hi, mult = param["min"], param["max"], param.get("view", {}).get("multiplier", 1.0)
        response = []
        for t in (0.0, 0.25, 0.5, 0.75, 1.0):
            put_value(param["id"], lo + t * (hi - lo))
            time.sleep(0.03)
            response.append(round(get_value(param["id"]) * mult, 6))
        put_value(param["id"], param["value"])
        out["response"] = response
    return out


def rest_name(effect):
    return effect.get("restName", effect["name"])


def discover(*names):
    spec = json.loads(SPEC.read_text())
    cached = "--cached" in names
    names = [n for n in names if n != "--cached"]
    wanted = [e for e in spec["effects"] if not names or e["name"] in names]
    found_file = REPO / "build/classic-discovery.json"
    found = json.loads(found_file.read_text()) if cached else {}
    if not cached:
        with Scratch() as s:
            for effect in wanted:
                e = s.add(rest_name(effect))
                found[effect["name"]] = {"params": {k: describe(p, True) for k, p in e["params"].items()}}
                s.clear_effects()
        found_file.parent.mkdir(exist_ok=True)
        found_file.write_text(json.dumps(found, indent=1))
    for effect in wanted:
        merge(effect, found[effect["name"]]["params"])
    SPEC.write_text(json.dumps(spec, indent=1) + "\n")
    print("updated", SPEC)


def camel(name):
    words = [w for w in "".join(c if c.isalnum() else " " for c in name).split()]
    first = words[0].lower() if words[0].isupper() else words[0][0].lower() + words[0][1:]
    return first + "".join(w[0].upper() + w[1:] for w in words[1:])


CHANNELS = ["Red", "Green", "Blue", "Hue", "Saturation", "Brightness", "Alpha"]


def display(rest):
    view = rest.get("view") or {}
    out = {k: rest[k] for k in ("min", "max", "options", "response") if k in rest}
    renames = {"multiplier": "multiplier", "suffix": "suffix", "display_units": "units", "control_type": "control",
               "step": "step", "alternative_name": "altName"}
    out.update({ours: view[key] for key, ours in renames.items() if view.get(key) not in (None, "")})
    return out


def kind_of(rest):
    return {"ParamRange": "range", "ParamBoolean": "bool", "ParamChoice": "choice", "ParamColor": "color"}.get(
        rest["type"], rest["type"])


def color_channels(name, hex_rgba):
    import colorsys
    r, g, b, a = (int(hex_rgba[i:i + 2], 16) / 255.0 for i in (1, 3, 5, 7))
    h, s, v = colorsys.rgb_to_hsv(r, g, b)
    values = dict(zip(CHANNELS, (r, g, b, h, s, v, a)))
    return [{"name": f"{name}.{c}", "label": c, "id": camel(name) + c, "kind": "range", "default": values[c],
             "stored": [0.0, 1.0], "display": {"min": 0.0, "max": 1.0}} for c in CHANNELS]


def merge(effect, rest_params):
    """Rewrites the effect's control list in the reference's order, keeping what the saved files told us."""
    known = {p["name"]: p for p in effect["params"]}
    merged = []
    for name, rest in rest_params.items():
        if name == "Opacity":
            continue
        kind = kind_of(rest)
        param = known.get(name) or {"name": name, "label": name, "id": camel(name), "kind": kind}
        param["display"] = display(rest)
        if kind == "color":
            r, g, b, a = (int(rest["default"][i:i + 2], 16) for i in (1, 3, 5, 7))
            param.setdefault("default", float(a << 24 | b << 16 | g << 8 | r))
            param.setdefault("packed", "uint32 0xAABBGGRR; channels follow as <name>.<channel>")
            merged.append(param)
            for channel in color_channels(name, rest["default"]):
                merged.append({**channel, **known.get(channel["name"], {}), "display": channel["display"]})
            continue
        if kind == "range" and not param.get("stored"):
            param["stored"] = [rest["min"], rest["max"]]
        if name not in known:
            if kind == "range":
                param["default"] = rest["default"]
            elif kind == "bool":
                param["default"] = 1.0 if rest["default"] else 0.0
            elif kind == "choice":
                param["default"] = float(rest["options"].index(rest["default"]))
        if "altName" in param["display"]:
            param["label"] = param["display"]["altName"]
        if kind == "choice":
            param["choice"] = {**param.get("choice", {}), "options": rest["options"], "source": "REST"}
        merged.append(param)
    effect["params"] = merged
    effect["controls"] = "complete: REST lists every control"


# Controls that move the picture over time; static sweeps leave them at their defaults.
MOVING = {"classicStripper": {"speed", "speedSpread"}}
PRESETS = Path("/Applications/Resolume Arena/default/Presets/Video Effects")
# Seconds between cases, and the most snapshots per second in a sequence: the reference app may be in use meanwhile.
PAUSE, SEQUENCE_FPS = 0.2, 20


def channel(p):
    """The colour a channel control belongs to (Color for Color.Red), or None for any other control."""
    color, _, name = p["name"].rpartition(".")
    return color if color and name in CHANNELS else None


def controls(effect):
    """Our controls of an effect with their REST counterparts: (our id, kind, stored range, default, REST name)."""
    out = []
    for p in effect["params"]:
        if p["kind"] == "color" or channel(p) and p["label"] in ("Hue", "Saturation", "Brightness"):
            continue
        rest = channel(p) or p["name"]
        out.append((p["id"], p["kind"], p.get("stored") or [0.0, 1.0], p["default"], rest, p))
    return out


def rest_values(effect, ours):
    """The REST values for a case: ranges as they are, booleans, choice option names and colours as #rrggbbaa."""
    values, colors = {}, {}
    for pid, kind, _, default, rest, p in controls(effect):
        v = ours.get(pid, default)
        if channel(p):
            colors.setdefault(rest, {})[p["label"]] = v
        elif kind == "bool":
            values[rest] = v > 0.5
        elif kind == "choice":
            values[rest] = p["display"]["options"][int(v)]
        else:
            values[rest] = v
    for rest, ch in colors.items():
        values[rest] = "#" + "".join("%02x" % round(ch[c] * 255) for c in ("Red", "Green", "Blue", "Alpha"))
    return values


def preset_values(effect, xml):
    """Static values of a bundled preset (animations dropped), its opacity and its mixer, by our ids."""
    import re
    text = Path(xml).read_text()
    param = re.compile(r'<(?:Param|ParamRange|ParamChoice) name="([^"]+)"[^>]*?value="([^"]+)"')
    outer, inner = text.split("<RenderPass", 2)[1:]
    by_name = {p["name"]: p["id"] for p in effect["params"]}
    values = {}
    for color, body in re.findall(r'<ParamColor name="([^"]+)"(.*?)</ParamColor>', inner, re.S):
        for name, value in param.findall(body):
            if f"{color}.{name}" in by_name:
                values[by_name[f"{color}.{name}"]] = float(value)
    for name, value in param.findall(re.sub(r"<ParamColor.*?</ParamColor>", "", inner, flags=re.S)):
        if name in by_name:
            values[by_name[name]] = float(value)
    opacity = float(dict(param.findall(outer)).get("Opacity", 1.0))
    blend = re.search(r'<ParamChoice name="Blend Mode"[^>]*?value="(\d+)"', text)
    mixer = {"67464114": "Add"}.get(blend.group(1), "Alpha") if blend else "Alpha"
    return values, opacity, mixer


# Effects that change over time on a still input: captured as timed sequences, not as one settled frame.
TIMED = {"classicTrails", "classicStrobe", "classicShiftGlitch"}
# Combinations beyond the one-control sweeps.
INTERACTIONS = {"classicMirrorQuad": {"flipXY.0": {"flipX": 1.0, "flipY": 1.0}},
                "classicTrails": {"modeFeedback.0": {"mode": 1.0, "feedback": 0.5},
                                  "modeFeedback.1": {"mode": 2.0, "feedback": 0.5}},
                "classicStrobe": {"fadeFrequency.0": {"fade": 1.0, "frequency": 0.25},
                                  "fadeFrequency.1": {"fade": 1.0, "frequency": 1.0}}}
# Controls a colour only shows through.
COLOR_NEEDS = {"classicEdgeDetection": {"useColor": 1.0}}
# The colours a colour control is swept over: six hues, black, and white at half alpha (red, green, blue, alpha).
HUES = [(1, 0, 0, 1), (1, 1, 0, 1), (0, 1, 0, 1), (0, 1, 1, 1), (0, 0, 1, 1), (1, 0, 1, 1), (0, 0, 0, 1),
        (1, 1, 1, 0.5)]


def range_points(lo, hi, step):
    """9 points over a range; a stepped range (step in stored units) with fewer positions gets each, otherwise the 9
    snap to the step."""
    if not step:
        return [lo + (hi - lo) * i / 8 for i in range(9)]
    positions = int(round((hi - lo) / step)) + 1
    if positions <= 9:
        return [lo + step * i for i in range(positions)]
    return sorted({lo + step * int(round((hi - lo) * i / 8 / step + 1e-9)) for i in range(9)})


def cases(work, *names):
    spec = json.loads(SPEC.read_text())
    source = names[names.index("--input") + 1] if "--input" in names else "input.png"
    names = [n for n in names if n not in ("--input", source)]
    out = []
    for effect in spec["effects"]:
        if effect["name"] not in names:
            continue
        type_ = effect["classicType"]

        def case(name, ours, mixer="Alpha", opacity=1.0):
            ours = dict(ours)
            if mixer != "Alpha":
                ours["blendMode"] = 1.0
            if opacity != 1.0:
                ours["mix"] = opacity
            out.append({"name": f"{type_}.{name}", "effect": effect["name"], "rest_name": rest_name(effect),
                        "type": type_, "params": ours, "rest": rest_values(effect, ours), "mixer": mixer,
                        "opacity": opacity, "input": source, "sequence": type_ in TIMED})

        case("default", {})
        for p in effect["params"]:
            if p["kind"] == "color":
                for i, rgba in enumerate(HUES):
                    ours = {p["id"] + c: v for c, v in zip(("Red", "Green", "Blue", "Alpha"), rgba)}
                    case(f"{p['id']}.{i}", {**COLOR_NEEDS.get(type_, {}), **ours})
        for pid, kind, (lo, hi), default, rest, p in controls(effect):
            if pid in MOVING.get(type_, ()) or channel(p):
                continue
            if kind == "bool":
                points = [0.0, 1.0]
            elif kind == "choice":
                points = [float(i) for i in range(len(p["display"]["options"]))]
            else:
                points = range_points(lo, hi, p["display"].get("step", 0) / p["display"].get("multiplier", 1))
            for i, v in enumerate(points):
                case(f"{pid}.{i}", {pid: v})
        for xml in sorted((PRESETS / effect["name"]).glob("*.xml")):
            values, opacity, mixer = preset_values(effect, xml)
            ids = {c[0] for c in controls(effect)} - MOVING.get(type_, set())
            values = {k: v for k, v in values.items() if k in ids}
            case("preset." + xml.stem.replace(" ", "_"), values, mixer, opacity)
        for name, ours in INTERACTIONS.get(type_, {}).items():
            case(name, ours)
    Path(work).mkdir(parents=True, exist_ok=True)
    (Path(work) / "cases.json").write_text(json.dumps({"input": source, "cases": out}, indent=1))
    print(len(out), "cases")


def set_case(effect, case):
    for name, value in case["rest"].items():
        put_value(effect["params"][name]["id"], value)
    put_value(effect["params"]["Opacity"]["id"], case["opacity"])
    put_value(effect["mixer"]["Blend Mode"]["id"], case["mixer"])


def capture(work, *only):
    work = Path(work)
    listing = json.loads((work / "cases.json").read_text())
    all_cases = listing["cases"]
    (work / "ref").mkdir(exist_ok=True)
    unstable = []
    with Scratch() as s:
        s.show(work / listing.get("input", "input.png"))
        (work / "ref/input.png").write_bytes(s.settled() or s.frame())
        current, effect = None, None
        for case in all_cases:
            if case.get("sequence") or only and not any(o in case["name"] for o in only):
                continue
            time.sleep(PAUSE)
            if case["effect"] != current:
                s.clear_effects()
                effect = s.add(case["rest_name"])
                current = case["effect"]
            set_case(effect, case)
            frame = s.settled()
            if frame is None:
                unstable.append(case["name"])
                frame = s.frame()
            (work / "ref" / (case["name"] + ".png")).write_bytes(frame)
    print("captured; still moving:", unstable)


def option(args, flag, default):
    return type(default)(args[args.index(flag) + 1]) if flag in args else default


def sequence(work, *args):
    """Timed reference frames of each time-based case, as fast as the snapshots come (at most SEQUENCE_FPS): the
    effect is bypassed while its values are set and switched on at t = 0, the timing marker; every frame records when
    it was asked for and when it arrived. --instances N repeats every case on N fresh effect instances."""
    work = Path(work)
    seconds, count = option(args, "--seconds", 4.0), option(args, "--instances", 1)
    only = [a for i, a in enumerate(args) if not a.startswith("--") and (i == 0 or args[i - 1] not in
                                                                        ("--seconds", "--instances"))]
    listing = json.loads((work / "cases.json").read_text())
    chosen = [c for c in listing["cases"] if c.get("sequence") and (not only or any(o in c["name"] for o in only))]
    with Scratch() as s:
        s.show(work / listing.get("input", "input.png"))
        (work / "ref").mkdir(exist_ok=True)
        (work / "ref/input.png").write_bytes(s.settled() or s.frame())
        tempo = (call("GET", "/composition").get("tempocontroller") or {}).get("tempo", {}).get("value")
        for i in range(count):
            current = None
            for case in chosen:
                if case["effect"] != current:
                    s.clear_effects()
                    effect = s.add(case["rest_name"])
                    current = case["effect"]
                time.sleep(PAUSE)
                put_value(effect["bypassed"]["id"], True)
                set_case(effect, case)
                time.sleep(0.3)
                out = work / "seq" / f"{case['name']}.{i}"
                out.mkdir(parents=True, exist_ok=True)
                frames = []
                put_value(effect["bypassed"]["id"], False)
                t0 = time.monotonic()
                while time.monotonic() - t0 < seconds:
                    asked = time.monotonic() - t0
                    png = call("GET", f"/composition/monitors/{s.monitor}/snapshot.png", raw=True)
                    came = time.monotonic() - t0
                    name = "%03d.png" % len(frames)
                    (out / name).write_bytes(png)
                    frames.append({"file": name, "asked": round(asked, 4), "came": round(came, 4)})
                    time.sleep(max(0.0, 1.0 / SEQUENCE_FPS - (came - asked)))
                (out / "frames.json").write_text(json.dumps(
                    {"case": case["name"], "instance": i, "params": case["params"], "rest": case["rest"],
                     "input": listing.get("input", "input.png"), "tempo": tempo, "seconds": seconds,
                     "marker": "t = 0 when the effect leaves bypass, its values already set", "frames": frames},
                    indent=1))
                print(f"{case['name']}.{i}: {len(frames)} frames")
    print("sequences captured:", len(chosen), "cases x", count, "instances")


# The marker clip: MOTION_FRAMES frames at MOTION_FPS, its frame number in MARKER_BITS blocks along the top (low bit
# first, white = 1).
MOTION_FRAMES, MOTION_FPS, MARKER_BITS = 120, 30, 8


def marker_block(bit):
    return slice(20, 80), slice(40 + bit * 100, 120 + bit * 100)


def inputs(out):
    """The probe inputs of phases 3 to 6: the UV picture (R = x, G = y, B = 128), a flat colour (200, 100, 50) with
    alpha rising from 64 at the top to 255 at the bottom, and the marker clip: a white disc swinging across, a red
    square bobbing, a grey ramp along the bottom and the frame number in blocks along the top."""
    import subprocess
    import numpy as np
    from PIL import Image
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    H, W = 1080, 1920
    x = np.arange(W)[None, :].repeat(H, 0).astype(float)
    y = np.arange(H)[:, None].repeat(W, 1).astype(float)
    uv = np.stack([x * 255 / (W - 1), y * 255 / (H - 1), np.full_like(x, 128.0), np.full_like(x, 255.0)], -1)
    Image.fromarray(np.round(uv).astype(np.uint8)).save(out / "uv.png")
    alpha = np.stack([np.full_like(y, 200.0), np.full_like(y, 100.0), np.full_like(y, 50.0), 64 + y * 191 / (H - 1)], -1)
    Image.fromarray(np.round(alpha).astype(np.uint8)).save(out / "alpha.png")
    ffmpeg = subprocess.Popen(["nice", "-n", "15", "ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo",
                               "-pix_fmt", "rgb24", "-s", f"{W}x{H}", "-r", str(MOTION_FPS), "-i", "-",
                               "-c:v", "libx264", "-crf", "4", "-pix_fmt", "yuv420p", "-g", "15",
                               str(out / "motion.mp4")], stdin=subprocess.PIPE)
    ramp = np.round(x[900:] * 255 / (W - 1))
    for f in range(MOTION_FRAMES):
        frame = np.full((H, W, 3), 24.0)
        frame[900:] = ramp[..., None]
        turn = 2 * np.pi * f / MOTION_FRAMES
        cx, cy = 960 + 720 * np.sin(turn), 450
        frame[(x - cx) ** 2 + (y - cy) ** 2 < 90 ** 2] = 255
        sy = int(450 + 300 * np.sin(2 * turn))
        frame[sy - 80:sy + 80, 220:380] = (255, 0, 0)
        for bit in range(MARKER_BITS):
            frame[marker_block(bit)] = 255 if f >> bit & 1 else 0
        ffmpeg.stdin.write(frame.astype(np.uint8).tobytes())
    ffmpeg.stdin.close()
    ffmpeg.wait()
    print("wrote", out / "uv.png", out / "alpha.png", out / "motion.mp4")


def clip_frame(rgb):
    """The marker clip's frame number read back from a frame, or None when the blocks are not clean."""
    bits = []
    for bit in range(MARKER_BITS):
        level = rgb[marker_block(bit)].mean()
        if 60 < level < 195:
            return None
        bits.append(level >= 128)
    return sum(1 << i for i, on in enumerate(bits) if on)


def timeline(work, *only):
    """Per frame of each sequence: its time, mean colour (over black), share changed from the bare input by more than
    8 levels, the logo overlay and, on the marker clip, its frame number. Writes timeline.json next to frames.json."""
    import io
    import numpy as np
    from PIL import Image
    work = Path(work)

    def load(png):
        a = np.asarray(Image.open(png).convert("RGBA")).astype(float)
        return a[..., :3] * a[..., 3:] / 255, a[..., 3].mean() / 255
    bare, _ = load(work / "ref/input.png")
    for folder in sorted((work / "seq").glob("*")):
        if only and not any(o in folder.name for o in only):
            continue
        meta = json.loads((folder / "frames.json").read_text())
        rows = []
        for f in meta["frames"]:
            data = (folder / f["file"]).read_bytes()
            rgb, alpha = load(io.BytesIO(data))
            rows.append({"t": round((f["asked"] + f["came"]) / 2, 4), "mean": [round(v, 2) for v in rgb.mean((0, 1))],
                         "alpha": round(alpha, 4), "changed": round(float((np.abs(rgb - bare).max(-1) > 8).mean()), 4),
                         "overlaid": overlaid(data), "clipFrame": clip_frame(rgb)})
        (folder / "timeline.json").write_text(json.dumps(rows, indent=0))
        luma = [0.2126 * r["mean"][0] + 0.7152 * r["mean"][1] + 0.0722 * r["mean"][2] for r in rows]
        span = rows[-1]["t"] - rows[0]["t"] if len(rows) > 1 else 0
        print(f"{folder.name:44} {len(rows):3} frames {len(rows) / max(span, 1e-6):5.1f}/s  luma {min(luma):6.1f}"
              f"..{max(luma):6.1f}  changed {min(r['changed'] for r in rows):.3f}..{max(r['changed'] for r in rows):.3f}")


def instances(work, count="5", effect="Stripper"):
    """More reference frames of each case of a randomly laid out effect, one fresh effect instance per round, into
    WORK/inst/NAME.I.png (ref/NAME.png is instance 0)."""
    work = Path(work)
    chosen = [c for c in json.loads((work / "cases.json").read_text())["cases"] if c["effect"] == effect]
    (work / "inst").mkdir(exist_ok=True)
    with Scratch() as s:
        s.show(work / "input.png")
        for i in range(1, int(count)):
            s.clear_effects()
            e = s.add(chosen[0]["rest_name"])
            for case in chosen:
                set_case(e, case)
                (work / "inst" / f"{case['name']}.{i}.png").write_bytes(s.snapshot() or s.frame())
    print("captured", len(chosen), "cases x", int(count) - 1, "instances")


def layout(png, card):
    """The share of the picture left as it was, and the strips (runs of changed pixels) across the middle row and
    column with their mean length in pixels."""
    import numpy as np
    from PIL import Image
    a = np.asarray(Image.open(png).convert("RGBA")).astype(float)
    changed = np.abs(a[..., :3] * a[..., 3:] / 255 - card).max(-1) > 8

    def runs(line):
        edges = np.flatnonzero(np.diff(np.concatenate(([0], line.astype(int), [0]))))
        lengths = edges[1::2] - edges[::2]
        return lengths[lengths > 3]
    row, col = runs(changed[changed.shape[0] // 2]), runs(changed[:, changed.shape[1] // 2])
    best = row if len(row) >= len(col) else col
    return {"shown": 1.0 - changed.mean(), "strips": len(best), "width": float(best.mean()) if len(best) else 0.0}


def strip_stats(work, *update):
    """Our frame of each case against the spread of the reference instances: share shown, strips and their width."""
    import numpy as np
    from PIL import Image
    work = Path(work)
    card = np.asarray(Image.open(work / "ref/input.png").convert("RGB")).astype(float)
    report, passing, total = {}, 0, 0
    for ref in sorted((work / "ref").glob("classicStripper.*.png")):
        name = ref.stem
        refs = [layout(p, card) for p in [ref, *sorted((work / "inst").glob(name + ".*.png"))]]
        ours = layout(work / "ours" / (name + ".png"), card)
        ok = {}
        for key, slack in (("shown", 0.05), ("strips", 1.0), ("width", 0.15)):
            lo, hi = min(r[key] for r in refs), max(r[key] for r in refs)
            pad = slack * (max(hi, 1.0) if key == "width" else 1.0)
            ok[key] = lo - pad <= ours[key] <= hi + pad
        group = name.split(".")[1]
        g = report.setdefault(group, {"cases": 0, "passing": 0})
        g["cases"] += 1
        g["passing"] += all(ok.values())
        passing += all(ok.values())
        total += 1
        if not all(ok.values()):
            print(f"  {name:40} ours {ours}  ref {[{k: round(v, 3) for k, v in r.items()} for r in refs]}")
    print(f"classicStripper statistics: {passing}/{total} within the reference instances")
    for group, g in report.items():
        print(f"   {group:16} {g['passing']}/{g['cases']}")
    if "--write" in update:
        out = REPO / "docs/research/classic-parity.json"
        old = json.loads(out.read_text())
        old["classicStripper"]["statistics"] = {
            "method": "our frame within the spread of the reference instances (share shown +-0.05, strips across the "
                      "middle row or column +-1, their mean width +-15 %)",
            "instances": 1 + len(list((work / "inst").glob(next((work / "ref").glob("classicStripper.*.png")).stem
                                                         + ".*.png"))),
            "pass": round(passing / total, 3), "cases": total, "byControl": report}
        out.write_text(json.dumps(old, indent=1) + "\n")


def render(work):
    import os
    import subprocess
    env = dict(os.environ, NOCTURNE_PARITY_DIR=str(Path(work).resolve()), NOCTURNE_PARITY_SAVE="1")
    subprocess.run(["nice", "-n", "15", str(REPO / "build/nocturne_tests"), "--test-case=parity frames*"],
                   env=env, check=True)


# The reference lays these out at random per instance: they pass on the share of the picture they leave as it was.
STATISTICAL = {"classicStripper"}


def passes(type_, s):
    if type_ in STATISTICAL:
        return abs(s["shown"] - s["shownRef"]) <= 0.1
    return s["mae"] <= 2.0 and s["ssim"] >= 0.98


def translucent(png):
    """The share of a picture's pixels that are not opaque."""
    import numpy as np
    from PIL import Image
    return float((np.asarray(Image.open(png).convert("RGBA"))[..., 3] < 255).mean())


def score(work, *update):
    import statistics
    work = Path(work)
    scores = json.loads((work / "scores.json").read_text())
    per_effect = {}
    print("input (the bare test card):", scores.pop("input", "not captured"))
    # The logo overlay leaves most of a still translucent, unlike the other stills of its input.
    shares = {n: translucent(work / "ref" / (n + ".png")) for n in scores}
    usual = statistics.median(shares.values()) if shares else 0.0
    spoiled = [n for n, v in shares.items() if v - usual > 0.25]
    if spoiled:
        print("left out, logo overlay in the reference:", ", ".join(spoiled))
    for name, s in scores.items():
        if name in spoiled:
            continue
        type_, rest = name.split(".", 1)
        group = rest.split(".")[0]
        e = per_effect.setdefault(type_, {"cases": 0, "passing": 0, "maes": [], "groups": {}})
        e["cases"] += 1
        e["passing"] += passes(type_, s)
        e["maes"].append(s["mae"])
        g = e["groups"].setdefault(group, {"cases": 0, "passing": 0, "worstMae": 0.0, "minSsim": 1.0,
                                           "worstShownGap": 0.0})
        g["cases"] += 1
        g["passing"] += passes(type_, s)
        g["worstMae"] = round(max(g["worstMae"], s["mae"]), 2)
        g["minSsim"] = round(min(g["minSsim"], s["ssim"]), 4)
        g["worstShownGap"] = round(max(g["worstShownGap"], abs(s["shown"] - s["shownRef"])), 3)
    report = {}
    for type_, e in per_effect.items():
        report[type_] = {"pass": round(e["passing"] / e["cases"], 3), "cases": e["cases"],
                         "medianMae": round(statistics.median(e["maes"]), 2), "byControl": e["groups"]}
        print(f"{type_}: {e['passing']}/{e['cases']} pass, median MAE {report[type_]['medianMae']}")
        for group, g in e["groups"].items():
            print(f"   {group:16} {g['passing']}/{g['cases']}  worst MAE {g['worstMae']:6.2f}  min SSIM {g['minSsim']:.4f}"
                  f"  worst shown gap {g['worstShownGap']}")
    if "--write" in update:
        out = REPO / "docs/research/classic-parity.json"
        old = json.loads(out.read_text()) if out.exists() else {}
        old.update(report)
        out.write_text(json.dumps(old, indent=1) + "\n")


def render_sequences(work, only=""):
    """Our sequences of WORK's timed cases (the parity sequences test case) into WORK/ours/seq/NAME.I.json."""
    import os
    import subprocess
    env = dict(os.environ, NOCTURNE_PARITY_DIR=str(Path(work).resolve()))
    if only:
        env["NOCTURNE_PARITY_ONLY"] = only
    subprocess.run(["nice", "-n", "15", str(REPO / "build/nocturne_tests"), "--test-case=parity sequences*"],
                   env=env, check=True)


def ref_sequences(work, prefix):
    """{case: [(frames.json, timeline.json) per reference instance]} for the sequences whose name starts with prefix."""
    out = {}
    for folder in sorted((Path(work) / "seq").glob(prefix + "*")):
        case = folder.name.rsplit(".", 1)[0]
        out.setdefault(case, []).append((json.loads((folder / "frames.json").read_text()),
                                         json.loads((folder / "timeline.json").read_text()), folder))
    return out


def ours_sequence(work, name):
    path = Path(work) / "ours/seq" / (name + ".json")
    return json.loads(path.read_text()) if path.exists() else None


def clean(row):
    """A snapshot of an opaque picture with no logo overlay, not even a fading one (which lowers the alpha first)."""
    return not row["overlaid"] and row["alpha"] >= 0.995


def luma(mean):
    return 0.2126 * mean[0] + 0.7152 * mean[1] + 0.0722 * mean[2]


def strobe_stats(work):
    """Each Strobe sequence against ours sampled every frame over 8 s: the time shift that best lines up our flashes with
    the reference's (the clock's phase at the marker is free), then each reference snapshot agrees when one of our
    frames between the moment it was asked for and the moment it came has the same flash state (or, with Fade, a level
    within 8 of it); snapshots with the logo overlay, even fading, are left out. A case passes when 90 % of its
    snapshots agree. The on and off levels: our brightest and darkest frames against the reference's."""
    import numpy as np
    report = {}
    for case, instances in ref_sequences(work, "classicStrobe").items():
        frames, rows, folder = instances[0]
        ours = ours_sequence(work, folder.name)
        if not ours:
            continue
        ot = np.array([s["t"] for s in ours["samples"]])
        ol = np.array([luma(s["mean"]) for s in ours["samples"]])
        keep = [(f, r) for f, r in zip(frames["frames"], rows) if clean(r)]
        rl = np.array([luma(r["mean"]) for _, r in keep])
        fade = frames["params"].get("fade", 0.0) > 0.5
        lo, hi = min(ol.min(), rl.min()), max(ol.max(), rl.max())
        mid = (lo + hi) / 2

        def agree(shift):
            hits = []
            for (f, r), level in zip(keep, rl):
                window = ol[(ot >= f["asked"] + shift - 0.5 / 60) & (ot <= f["came"] + shift + 0.5 / 60)]
                if not len(window):
                    hits.append(False)
                elif fade:
                    hits.append(window.min() - 8 <= level <= window.max() + 8)
                else:
                    hits.append(bool(np.any((window > mid) == (level > mid))))
            return float(np.mean(hits))
        shifts = np.arange(0.0, 4.0, 1 / 120)
        scores = [agree(s) for s in shifts]
        best = int(np.argmax(scores))
        group = case.split(".")[1]
        report[case] = {"agree": round(scores[best], 3), "shift": round(float(shifts[best]), 3),
                        "pass": scores[best] >= 0.9, "group": group,
                        "levels": None if fade else {"on": ours["bright"], "off": ours["dark"]}}
        lv = report[case]["levels"]
        lv_ok = lv is None or all(v["mae"] <= 2.0 and v["ssim"] >= 0.98 for v in lv.values())
        report[case]["levelsPass"] = lv_ok
        print(f"  {case:34} timing {scores[best]:.2f} at shift {shifts[best]:.3f}"
              + ("" if lv is None else f"  on MAE {lv['on']['mae']:.2f} SSIM {lv['on']['ssim']:.4f}"
                 f"  off MAE {lv['off']['mae']:.2f} SSIM {lv['off']['ssim']:.4f}"))
    return report


def band_runs(level, n):
    """Runs of rows (or columns) read from elsewhere on the UV probe: (start, size, slope in source lines per line)."""
    import numpy as np
    level = np.asarray(level, dtype=float)
    own = np.round(np.arange(n) * 255.0 / (n - 1))
    moved = np.abs(level - own) > 1.5
    out, i = [], 0
    while i < n:
        if not moved[i]:
            i += 1
            continue
        j = i + 1
        while j < n and moved[j] and abs(level[j] - level[j - 1]) <= 2.0:
            j += 1
        slope = float(np.polyfit(np.arange(i, j), level[i:j] * (n - 1) / 255.0, 1)[0]) if j - i >= 8 else None
        out.append((i, j - i, slope))
        i = j
    return out, float(moved.mean())


def band_summary(lines):
    """Moved share, bands per frame, median band size and slope over frames given as (rowG, colR) medians."""
    import numpy as np
    out = {}
    for axis, key in ((0, "rows"), (1, "columns")):
        runs, moved = [], []
        for line in lines:
            r, m = band_runs(line[axis], len(line[axis]))
            runs += r
            moved.append(m)
        slopes = [s for _, _, s in runs if s is not None]
        out[key] = {"moved": float(np.mean(moved)), "bands": len(runs) / max(1, len(lines)),
                    "size": float(np.median([n for _, n, _ in runs])) if runs else 0.0,
                    "slope": float(np.median(slopes)) if len(slopes) >= 10 else None}
    return out


def ref_lines(folder, frames, rows):
    """The UV reference sequence's per-frame row G and column R medians, cached next to it in WORK/ours."""
    import numpy as np
    from PIL import Image
    cache = folder.parent.parent / "ours" / "seq" / (folder.name + ".ref-lines.json")
    if cache.exists():
        return json.loads(cache.read_text())
    lines = []
    for f, r in zip(frames["frames"], rows):
        if not clean(r):
            continue
        a = np.asarray(Image.open(folder / f["file"]).convert("RGB"))
        lines.append([np.median(a[:, :, 1], axis=1).tolist(), np.median(a[:, :, 0], axis=0).tolist()])
    cache.parent.mkdir(parents=True, exist_ok=True)
    cache.write_text(json.dumps(lines))
    return lines


UV_INSTANCES = 5


def glitch_stats(card, uv):
    """Shift Glitch cases on statistics, snapshots with the logo overlay (even fading) left out. A case passes on the
    test card's five reference instances against five of ours, the mean share changed (medians within 0.03 or 20 %; ours
    less what our card changes with no band, Horizontal 0, as its marker sweeps and the reference's card is still),
    and on the UV probe (still, unlike our card's moving marker) the share of snapshot pairs whose pattern changed
    (within 0.05 or 20 % of our median). Its band shape is reported apart, from the UV probe's one reference instance
    against the spread of five of ours: rows and columns each, the moved share, bands per frame and their median size
    and slope (given 10 bands with a slope), each within the range of ours padded by 0.03, 12.5 % (at least 0.5),
    17.5 % + 1 and 17.5 %."""
    import statistics
    report = {}
    card_refs = ref_sequences(card, "classicShiftGlitch")
    uv_refs = ref_sequences(uv, "classicShiftGlitch")
    still = [statistics.mean(s["changed"] for s in o["samples"])
             for o in (ours_sequence(card, f"classicShiftGlitch.horizontal.0.{i}") for i in range(5)) if o]
    baseline = statistics.mean(still) if still else 0.0
    for case, instances in card_refs.items():
        checks, detail = {}, {}
        ref_changed, our_changed = [], []
        for frames, rows, folder in instances:
            if not any(clean(r) for r in rows):
                continue
            ref_changed.append(statistics.mean(r["changed"] for r in rows if clean(r)))
            ours = ours_sequence(card, folder.name)
            if ours:
                our_changed.append(max(0.0, statistics.mean(s["changed"] for s in ours["samples"]) - baseline))
        if not our_changed:
            continue
        stats = [("changed", ref_changed, our_changed, 0.03)]
        for frames, rows, folder in uv_refs.get(case, []):
            lines = ref_lines(folder, frames, rows)
            ours = [o for o in (ours_sequence(uv, f"{case}.{i}") for i in range(UV_INSTANCES)) if o]
            if len(lines) < 10 or not ours:
                continue
            ref_rate = sum(a != b for a, b in zip(lines, lines[1:])) / max(1, len(lines) - 1)
            our_rate = [sum(not s["same"] for s in o["samples"][1:]) / max(1, len(o["samples"]) - 1) for o in ours]
            stats.append(("rate", [ref_rate], our_rate, 0.05))
            ref_b = band_summary(lines)
            our_b = [band_summary([(s["rowG"], s["colR"]) for s in o["samples"]]) for o in ours]
            for axis in ("rows", "columns"):
                r = ref_b[axis]
                for key, pad in (("moved", lambda v: 0.03), ("bands", lambda v: max(0.5, 0.125 * v)),
                                 ("size", lambda v: 0.175 * v + 1), ("slope", lambda v: 0.175 * v)):
                    values = [o[axis][key] for o in our_b if o[axis][key] is not None]
                    if r[key] is None or len(values) < len(our_b) or (key == "size" and r["bands"] < 1.0):
                        continue
                    checks[f"{axis}.{key}"] = min(values) - pad(r[key]) <= r[key] <= max(values) + pad(r[key])
                detail["uv." + axis] = {"ref": {k: v if v is None else round(v, 3) for k, v in r.items()},
                                        "ours": [{k: v if v is None else round(v, 3) for k, v in o[axis].items()}
                                                 for o in our_b]}
        for key, ref, ours, abs_tol in stats:
            r, o = statistics.median(ref), statistics.median(ours)
            pad = max(abs_tol, 0.2 * r)
            checks[key] = abs(o - r) <= pad
            detail[key] = {"ref": round(r, 3), "ours": round(o, 3), "refInstances": [round(v, 3) for v in ref]}
        primary = [k for k in ("changed", "rate") if k in checks]
        report[case] = {"pass": all(checks[k] for k in primary),
                        "shape": all(v for k, v in checks.items() if k not in primary), "checks": checks,
                        "detail": detail, "group": case.split(".")[1]}
        failed = [k for k, v in checks.items() if not v]
        print(f"  {case:40} {'pass' if report[case]['pass'] else 'FAIL'}"
              + (f"  (off: {', '.join(failed)})" if failed else ""))
    return report


def trails_model(mode, feedback):
    """The Trails history update of one 60 Hz frame, as the shader does it, on 8-bit pictures."""
    import numpy as np
    weights = np.array([0.299, 0.587, 0.114])

    def update(c, kept):
        if kept is None:
            return c.copy()
        if mode == 1:
            return np.where((kept @ weights / 255 > 1 - feedback)[..., None], kept, c)
        m = np.round(c + (kept - c) * np.sqrt(feedback))
        return np.maximum(c, m) if mode == 2 else m
    return update


def trails_phase(work, *only):
    """The clip phase of each Trails sequence (clip frame = floor(30 x (t + phase))), fitted with a quarter-size
    simulation of the measured model against the snapshots after 1.5 s, into WORK/phase.json."""
    import subprocess
    import tempfile
    import numpy as np
    from PIL import Image
    work = Path(work)
    listing = json.loads((work / "cases.json").read_text())
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(["nice", "-n", "15", "ffmpeg", "-loglevel", "error", "-i", str(work / listing["input"]),
                        "-vf", "scale=480:270:flags=area", str(Path(tmp) / "%03d.png")], check=True)
        clip = [np.asarray(Image.open(p).convert("RGB")).astype(float) for p in sorted(Path(tmp).glob("*.png"))]

    def small(png):
        return np.asarray(Image.open(png).convert("RGB").resize((480, 270), Image.BOX)).astype(float)
    phases = json.loads((work / "phase.json").read_text()) if (work / "phase.json").exists() else {}
    for case in listing["cases"]:
        if not case.get("sequence") or (only and not any(o in case["name"] for o in only)):
            continue
        params = {"feedback": 0.9, "mode": 0.0, **case["params"]}
        update = trails_model(int(params["mode"]), params["feedback"])
        # A Luma history keeps what it took from the first frame on, so it starts empty at switch-on.
        first = 0 if int(params["mode"]) == 1 else -90
        for folder in sorted((work / "seq").glob(case["name"] + ".*")):
            meta = json.loads((folder / "frames.json").read_text())
            rows = json.loads((folder / "timeline.json").read_text())
            snaps = [(r["t"], small(folder / f["file"])) for f, r in zip(meta["frames"], rows)
                     if not r["overlaid"] and r["t"] >= 1.5][::5]

            def fit(c):
                kept, sims = None, {}
                for n in range(first, int((snaps[-1][0] + 0.1) * 60) + 1):
                    kept = update(clip[int(np.floor(30 * (n / 60 + c))) % len(clip)], kept)
                    if n / 60 > 1.4:
                        sims[n] = kept
                return float(np.median([min(np.abs(ref - s).mean() for n, s in sims.items() if abs(n / 60 - t) <= 0.07)
                                        for t, ref in snaps]))
            coarse = min((fit(c), c) for c in np.arange(0.0, 4.0, 1 / 30))
            best = min((fit(coarse[1] + d), coarse[1] + d) for d in np.arange(-1 / 30, 1 / 30 + 1e-9, 1 / 120))
            phases[folder.name] = round(float(best[1] % 4.0), 4)
            print(f"  {folder.name:36} phase {phases[folder.name]:.3f} (quarter-size median MAE {best[0]:.2f})")
    (work / "phase.json").write_text(json.dumps(phases, indent=1))


# Their history comes from the reference instance's earlier cases (Feedback 1 freezes it, Luma keeps whatever is
# lighter than 1 - Feedback), so their frames cannot be matched from a fresh start.
INHERITED = {"classicTrails.feedback.8", "classicTrails.mode.1", "classicTrails.modeFeedback.0"}


def trails_stats(work):
    """Each Trails snapshot after 1.5 s (and 1 s after the reference's clip last stood still; a fading logo overlay left
    out) against our frame of the same clip time: it passes at MAE <= 2 and SSIM >= 0.98, a case at 90 % of its
    snapshots; the single-frame check is its snapshot of median MAE. Sequences whose history the reference inherited
    are checked on what does not depend on it: Feedback 1 holds a still picture in both, and in Luma every pixel lighter
    than 1 - Feedback stays as it was between the reference's snapshots. Feedback 1 holds whatever picture the effect
    started on, so it is always checked as a still picture."""
    import numpy as np
    from PIL import Image
    report = {}
    for case, instances in ref_sequences(work, "classicTrails").items():
        frames, rows, folder = instances[0]
        ours = ours_sequence(work, folder.name)
        if not ours:
            continue
        group = case.split(".")[1]
        params = {"feedback": 0.9, "mode": 0.0, **frames["params"]}
        if case in INHERITED or params["feedback"] >= 1.0:
            kept = [f for f, r in zip(frames["frames"], rows) if not r["overlaid"]]
            if params["feedback"] >= 1.0:
                ref_still = len({(folder / f["file"]).read_bytes() for f in kept}) == 1
                our_still = all(s["same"] for s in ours["samples"][2:])
                report[case] = {"pass": ref_still and our_still, "group": group, "check": "still picture",
                                "refStill": ref_still, "oursStill": our_still}
            else:
                held, total = 0, 0
                previous = None
                for f in kept:
                    a = np.asarray(Image.open(folder / f["file"]).convert("RGB")).astype(float)
                    if previous is not None:
                        light = previous @ np.array([0.299, 0.587, 0.114]) / 255 > 1 - params["feedback"] + 0.004
                        held += int(np.sum(np.all(a == previous, axis=-1) & light))
                        total += int(np.sum(light))
                    previous = a
                share = held / max(1, total)
                report[case] = {"pass": share >= 0.999, "group": group, "check": "lighter pixels kept",
                                "keptShare": round(share, 5)}
            print(f"  {case:34} {report[case]}")
            continue
        fading = {f["file"] for f, r in zip(frames["frames"], rows) if not clean(r)}
        # Where the reference's clip stood still (the same frame number in consecutive snapshots), its history holds
        # that frame for a while after the clip moves on. Lighten keeps the white marker blocks, so only Alpha reads.
        alpha_mode = frames["params"].get("mode", 0.0) == 0.0
        seen = [r for r in rows if clean(r) and alpha_mode]
        stalled = [b["t"] for a, b in zip(seen, seen[1:]) if a["clipFrame"] is not None and a["clipFrame"] == b["clipFrame"]]
        settle = max([1.5] + [t + 1.0 for t in stalled])
        matches = [m for m in ours.get("matches", []) if m["file"] not in fading and m["t"] >= settle]
        ok = [m["mae"] <= 2.0 and m["ssim"] >= 0.98 for m in matches]
        last = sorted(matches, key=lambda m: m["mae"])[len(matches) // 2] if matches else None
        report[case] = {"pass": bool(matches) and bool(np.mean(ok) >= 0.9), "group": group, "snapshots": len(matches),
                        "passing": int(sum(ok)), "medianMae": round(float(np.median([m["mae"] for m in matches])), 2),
                        "worstMae": round(max(m["mae"] for m in matches), 2),
                        "minSsim": round(min(m["ssim"] for m in matches), 4),
                        "frame": {"t": last["t"], "mae": round(last["mae"], 2), "ssim": round(last["ssim"], 4),
                                  "pass": bool(last["mae"] <= 2.0 and last["ssim"] >= 0.98)}}
        r = report[case]
        print(f"  {case:34} {r['passing']}/{r['snapshots']} snapshots, median MAE {r['medianMae']}, worst "
              f"{r['worstMae']}, min SSIM {r['minSsim']}; median frame at {last['t']:.2f}: MAE {last['mae']:.2f} "
              f"SSIM {last['ssim']:.4f}")
    return report


def timed(*args):
    """Scores the time-based Classic effects from the phase 6 work directories (card, uv, motion under P6)."""
    p6 = Path(args[0])
    reports = {"classicStrobe": strobe_stats(p6 / "card"), "classicShiftGlitch": glitch_stats(p6 / "card", p6 / "uv"),
               "classicTrails": trails_stats(p6 / "motion")}
    summary = {}
    for type_, report in reports.items():
        if not report:
            continue
        groups = {}
        for case, r in report.items():
            g = groups.setdefault(r["group"], {"cases": 0, "passing": 0})
            g["cases"] += 1
            g["passing"] += bool(r["pass"])
        passing = sum(bool(r["pass"]) for r in report.values())
        summary[type_] = {"pass": round(passing / len(report), 3), "cases": len(report), "byControl": groups,
                          "cases_": report}
        print(f"{type_}: {passing}/{len(report)} cases pass")
        if type_ == "classicStrobe":
            lv = [r["levelsPass"] for r in report.values() if r["levels"] is not None]
            summary[type_]["levels"] = {"pass": round(sum(lv) / len(lv), 3), "cases": len(lv)}
            print(f"   on/off levels: {sum(lv)}/{len(lv)} cases at MAE <= 2 and SSIM >= 0.98")
        if type_ == "classicShiftGlitch":
            shape = {}
            for r in report.values():
                for key, ok in r["checks"].items():
                    if key not in ("changed", "rate"):
                        c = shape.setdefault(key, [0, 0])
                        c[0] += ok
                        c[1] += 1
            summary[type_]["shape"] = {k: f"{a}/{b}" for k, (a, b) in shape.items()}
            print("   band shape on the UV probe:", summary[type_]["shape"])
        if type_ == "classicTrails":
            fr = [r["frame"]["pass"] for r in report.values() if "frame" in r]
            summary[type_]["frame"] = {"pass": round(sum(fr) / len(fr), 3), "cases": len(fr)}
            print(f"   single frame: {sum(fr)}/{len(fr)} cases at MAE <= 2 and SSIM >= 0.98")
    if "--write" in args:
        out = REPO / "docs/research/classic-parity.json"
        old = json.loads(out.read_text())
        methods = {"classicStrobe": strobe_stats, "classicShiftGlitch": glitch_stats, "classicTrails": trails_stats}
        for type_, s in summary.items():
            s["method"] = " ".join(methods[type_].__doc__.split())
            s["perCase"] = {case.split(".", 1)[1]: compact(r) for case, r in s.pop("cases_").items()}
            old[type_] = s
        out.write_text(json.dumps(old, indent=1) + "\n")


def compact(r):
    """A case's report without the bulky detail, numbers rounded."""
    out = {k: v for k, v in r.items() if k not in ("detail", "checks", "group")}
    if "checks" in r:
        out["off"] = [k for k, ok in r["checks"].items() if not ok]
    for key in ("levels",):
        if out.get(key):
            out[key] = {k: {m: round(x, 3) for m, x in v.items() if m in ("mae", "ssim")} for k, v in out[key].items()}
    return out


if __name__ == "__main__":
    sys.exit(main() or 0)
