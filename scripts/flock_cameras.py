#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Known ALPR cameras for Flock You's map layer, from OpenStreetMap.

DeFlock (deflock.me) maps license-plate readers into OpenStreetMap as
man_made=surveillance + surveillance:type=ALPR. This pulls them for an area
from the Overpass API and writes the cameras.csv the radio reads:

    lat,lon,direction,label
    47.610012,-122.339871,270,Flock Safety

Put the file in the radio's data folder as flock-you/cameras.csv (the SD card's
meshcomod/flock-you/ when the radio keeps its data there), then open Flock You >
Cameras > Reload. Up to 40,000 cameras fit.

Examples:
    scripts/flock_cameras.py --bbox 47.0,-123.0,48.2,-121.5 -o cameras.csv   # Puget Sound
    scripts/flock_cameras.py --area "Washington" -o cameras.csv              # a whole state
    scripts/flock_cameras.py --area "Oregon" --flock-only -o cameras.csv
    scripts/flock_cameras.py --from-json saved_overpass.json -o cameras.csv  # offline

Standard library only.
"""

import argparse
import json
import re
import sys
import urllib.parse
import urllib.request

OVERPASS = "https://overpass-api.de/api/interpreter"
CARDINAL = {"N": 0, "NNE": 22, "NE": 45, "ENE": 67, "E": 90, "ESE": 112, "SE": 135, "SSE": 157,
            "S": 180, "SSW": 202, "SW": 225, "WSW": 247, "W": 270, "WNW": 292, "NW": 315, "NNW": 337}


def query(scope: str, flock_only: bool) -> str:
    vendor = '["manufacturer"~"Flock",i]' if flock_only else ""
    sel = f'node["man_made"="surveillance"]["surveillance:type"="ALPR"]{vendor}'
    if scope.startswith("bbox:"):
        return f"[out:json][timeout:300];{sel}({scope[5:]});out body;"
    name = scope[5:].replace('"', '\\"')
    return (f'[out:json][timeout:300];area["name"="{name}"]["boundary"="administrative"]->.a;'
            f"{sel}(area.a);out body;")


def fetch(q: str, url: str) -> dict:
    data = urllib.parse.urlencode({"data": q}).encode()
    req = urllib.request.Request(url, data=data, headers={"User-Agent": "wadamesh-flock-you/2.0"})
    with urllib.request.urlopen(req, timeout=330) as r:
        return json.load(r)


def direction(tags: dict) -> str:
    """Degrees the camera faces, from direction / camera:direction; '' when unknown.
    Several directions ("90;270") give the first; a range ("45-90") its middle."""
    for key in ("direction", "camera:direction"):
        v = (tags.get(key) or "").strip()
        if not v:
            continue
        v = v.split(";")[0].strip()
        if v.upper() in CARDINAL:
            return str(CARDINAL[v.upper()])
        m = re.fullmatch(r"(\d+(?:\.\d+)?)\s*-\s*(\d+(?:\.\d+)?)", v)
        if m:
            a, b = float(m.group(1)), float(m.group(2))
            if b < a:
                b += 360
            return str(int(round((a + b) / 2)) % 360)
        try:
            return str(int(round(float(v))) % 360)
        except ValueError:
            continue
    return ""


def label(tags: dict) -> str:
    s = tags.get("manufacturer") or tags.get("brand") or tags.get("operator") or "ALPR"
    return re.sub(r"[,\r\n]", " ", s).strip()[:32].strip()


def rows(osm: dict):
    seen = set()
    for el in osm.get("elements", []):
        if el.get("type") != "node" or "lat" not in el:
            continue
        key = (round(el["lat"], 6), round(el["lon"], 6))
        if key in seen:
            continue
        seen.add(key)
        tags = el.get("tags", {})
        yield f'{el["lat"]:.6f},{el["lon"]:.6f},{direction(tags)},{label(tags)}'


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--bbox", help="south,west,north,east in degrees")
    g.add_argument("--area", help='an administrative area by name, e.g. "Washington"')
    g.add_argument("--from-json", help="an Overpass JSON answer saved earlier")
    ap.add_argument("--flock-only", action="store_true", help="only cameras tagged manufacturer=Flock Safety")
    ap.add_argument("--overpass", default=OVERPASS, help="Overpass API endpoint")
    ap.add_argument("-o", "--out", default="cameras.csv")
    a = ap.parse_args()

    if a.from_json:
        with open(a.from_json, encoding="utf-8") as f:
            osm = json.load(f)
    else:
        if a.bbox:
            parts = [float(x) for x in a.bbox.split(",")]
            if len(parts) != 4 or parts[0] >= parts[2] or parts[1] >= parts[3]:
                ap.error("--bbox wants south,west,north,east")
            scope = "bbox:" + ",".join(str(x) for x in parts)
        else:
            scope = "area:" + a.area
        print("asking Overpass (this can take a minute)...", file=sys.stderr)
        osm = fetch(query(scope, a.flock_only), a.overpass)

    out = list(rows(osm))
    if len(out) > 40000:
        print(f"note: {len(out)} cameras; the radio keeps the first 40000", file=sys.stderr)
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("lat,lon,direction,label\n")
        for r in out:
            f.write(r + "\n")
    print(f"{len(out)} cameras -> {a.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
