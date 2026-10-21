#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The rows test/test_flock_core.cpp gives the radio's downloader
(flockyou::osmRowToCamLine), run through scripts/flock_cameras.py: the radio
and the script must write the same cameras.csv line for the same camera."""

import importlib.util
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("flock_cameras", ROOT / "scripts" / "flock_cameras.py")
fc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fc)

KEYS = ["direction", "camera:direction", "manufacturer", "brand", "operator"]

# Overpass CSV row -> what the radio writes (as in test_flock_core.cpp's download()).
CASES = [
    ("47.6100120|-122.3398710|270|||Flock Safety|", "47.610012,-122.339871,270,Flock Safety"),
    ("47.61|-122.33||NW|Motorola|Vigilant|City", "47.610000,-122.330000,315,Motorola"),
    ("47.61|-122.33|45-90|||Flock Safety|", "47.610000,-122.330000,68,Flock Safety"),
    ("47.61|-122.33|350-10||||", "47.610000,-122.330000,0,ALPR"),
    ("47.61|-122.33|90;270|||City of Seattle|", "47.610000,-122.330000,90,City of Seattle"),
    ("47.61|-122.33|bogus|180|Flock, Inc.||", "47.610000,-122.330000,180,Flock  Inc."),
    ("47.61|-122.33|-90||||", "47.610000,-122.330000,270,ALPR"),
    ("47.61|-122.33", "47.610000,-122.330000,,ALPR"),
    ("47.61|-122.33|||  Flock Safety Falcon With A Very Long Name Indeed  ||",
     "47.610000,-122.330000,,Flock Safety Falcon With A Very"),
]


def main() -> int:
    bad = 0
    for row, want in CASES:
        f = row.split("|", 6) + [""] * 7
        tags = {k: v for k, v in zip(KEYS, f[2:7]) if v}
        osm = {"elements": [{"type": "node", "lat": float(f[0]), "lon": float(f[1]), "tags": tags}]}
        got = list(fc.rows(osm))
        if got != [want]:
            print(f"FAIL {row!r}: script {got} radio {want!r}")
            bad += 1
    print("flock download: script and radio agree" if not bad else f"{bad} failed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
