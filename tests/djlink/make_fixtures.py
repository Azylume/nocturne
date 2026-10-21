#!/usr/bin/env python3
# Writes the Pro DJ Link packet fixtures from the layouts in Deep Symmetry's protocol analysis
# (https://djl-analysis.deepsymmetry.org/), not from a capture: no recording of real CDJs exists yet.
import pathlib
import sys

HERE = pathlib.Path(__file__).parent
sys.path.insert(0, str(HERE.parent.parent / "tools" / "djlink-sim"))

from djlink_packets import beat, cdj_status, keep_alive, mixer_status, on_air  # noqa: E402

FIXTURES = {
    "keepalive_cdj2.bin": keep_alive(2, "CDJ-2000NXS2", [0x00, 0xe0, 0x36, 0x01, 0x02, 0x03], [169, 254, 10, 2], 1),
    "keepalive_djm.bin": keep_alive(0x21, "DJM-900NXS2", [0x00, 0xe0, 0x36, 0x0a, 0x0b, 0x0c], [169, 254, 10, 33], 2),
    # 126 BPM at +1.5 % pitch, the third beat of the bar.
    "beat_cdj2.bin": beat(2, 126.0, 0x100000 + round(0x100000 * 0.015), 3),
    "onair_4ch.bin": on_air([0, 1, 0, 0]),
    "onair_6ch.bin": on_air([0, 0, 0, 0, 0, 1]),
    # Playing, master and on air, rekordbox track 4242 from its own USB.
    "status_cdj2.bin": cdj_status(2, 4242, 126.0, 0x100000, 517, 2, 0x03, 0x40 | 0x20 | 0x08),
    "status_djm.bin": mixer_status(126.0, 4, False),
}

for file, data in FIXTURES.items():
    (HERE / file).write_bytes(data)
