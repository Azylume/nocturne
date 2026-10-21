# Pro DJ Link packet builders, from the layouts in Deep Symmetry's protocol analysis
# (https://djl-analysis.deepsymmetry.org/). Shared by tests/djlink/make_fixtures.py and the simulator.
import struct

MAGIC = bytes.fromhex("5173707431576d4a4f4c")
PITCH_NORMAL = 0x100000
CDJ = "CDJ-2000NXS2"
DJM = "DJM-900NXS2"


def name(text):
    return text.encode().ljust(20, b"\0")


def status_header(kind, device_name, subtype, number, total):
    return MAGIC + bytes([kind]) + name(device_name) + bytes([1, subtype, number]) + struct.pack(">H", total - 0x24)


def keep_alive(number, device_name, mac, ip, device_type):
    p = MAGIC + bytes([0x06, 0]) + name(device_name) + bytes([1, 2]) + struct.pack(">H", 0x36)
    p += bytes([number, device_type]) + bytes(mac) + bytes(ip) + bytes([3, 0, 0, 0, device_type, 0])
    assert len(p) == 0x36
    return p


def beat(number, bpm, pitch, beat_in_bar, device_name=CDJ, interval_ms=None):
    interval = interval_ms if interval_ms is not None else round(60000 / bpm)
    p = status_header(0x28, device_name, 0, number, 0x60)
    p += struct.pack(">6I", interval, 2 * interval, (5 - beat_in_bar) * interval, 4 * interval,
                     (9 - beat_in_bar) * interval, 8 * interval)
    p += b"\xff" * 0x18
    p += struct.pack(">I", pitch) + b"\0\0" + struct.pack(">H", round(bpm * 100)) + bytes([beat_in_bar, 0, 0, number])
    assert len(p) == 0x60
    return p


def on_air(flags, device_name=DJM):
    six = len(flags) == 6
    total = 0x35 if six else 0x2d
    p = status_header(0x03, device_name, 3 if six else 0, 0x21, total)
    p += bytes(flags[:4]) + bytes(5)
    if six:
        p += bytes(flags[4:]) + bytes(6)
    assert len(p) == total
    return p


# `bpm` and `beat_count` None: no analysed track (no beat grid).
def cdj_status(number, track_id, bpm, pitch, beat_count, beat_in_bar, play_state, flags, device_name=CDJ):
    p = bytearray(status_header(0x0a, device_name, 3, number, 0xd4))
    p += bytes(0xd4 - len(p))
    p[0x24] = number
    p[0x27] = 1
    p[0x28], p[0x29], p[0x2a] = number, 3, 1
    p[0x2c:0x30] = struct.pack(">I", track_id)
    p[0x7b] = play_state
    p[0x89] = 0x84 | flags
    p[0x8b] = 0xfa if flags & 0x40 else 0xfe
    p[0x8c:0x90] = struct.pack(">I", pitch)
    p[0x92:0x94] = struct.pack(">H", 0xffff if bpm is None else round(bpm * 100))
    p[0x98:0x9c] = struct.pack(">I", pitch)
    p[0xa0:0xa4] = struct.pack(">I", 0xffffffff if beat_count is None else beat_count)
    p[0xa6] = beat_in_bar
    p[0xc0:0xc4] = struct.pack(">I", pitch)
    p[0xc4:0xc8] = struct.pack(">I", pitch)
    return bytes(p)


def mixer_status(bpm, beat_in_bar, master, device_name=DJM):
    p = status_header(0x29, device_name, 0, 0x21, 0x38)
    p += bytes([0x21, 0, 0, 0xf0 if master else 0xd0]) + struct.pack(">I", 0x100000) + bytes([0x80, 0])
    p += struct.pack(">H", round(bpm * 100)) + bytes([0, 0x10, 0, 0, 0, 9, 0xff, beat_in_bar])
    assert len(p) == 0x38
    return p
