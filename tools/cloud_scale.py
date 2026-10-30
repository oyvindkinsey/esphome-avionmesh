#!/usr/bin/env python3
"""Synthetic Avi-on cloud inventory generator for sync scaling analysis.

Generates realistic abstract_devices / groups payloads at various mesh
sizes and measures their JSON byte cost, so the on-device sync buffer
(256KB cap) and ArduinoJson duplication (~1:1) can be reasoned about
without touching real account data.

Usage:
    tools/cloud_scale.py [--devices N ...] [--write DIR]

Device-side runs additionally need the firmware to talk to something
other than the fixed https://api.avi-on.com/ host; until PR #26 offers
a test override for the base URL, these numbers feed analysis, not a
live device run.
"""

import argparse
import json
import random

# Field shape mirrors what cloud_parse_devices / cloud_parse_group accept.
NAMES = [
    "Living Room Sconces",
    "Dining Room Ceiling 1",
    "Kitchen, Pendants",
    "Primary, bedside, right",
    "Front Yard",
    "Hallway Downlight",
    "Office Desk Lamp",
    "Garage Utility",
]
PRODUCTS = [90, 93, 94, 97, 134, 137, 162, 167]


def fake_pid(rng):
    return "".join(rng.choice("0123456789abcdef") for _ in range(24))


def make_inventory(n_devices, n_groups, seed=1):
    rng = random.Random(seed)
    devices = []
    for i in range(n_devices):
        devices.append(
            {
                "type": "device",
                "avid": 32768 + i,
                "pid": fake_pid(rng),
                "name": f"{rng.choice(NAMES)} {i}",
                "product_id": rng.choice(PRODUCTS),
            }
        )
    groups = []
    for g in range(n_groups):
        members = [d["pid"] for d in rng.sample(devices, min(4, n_devices))]
        groups.append(
            {
                "avid": 256 + g,
                "pid": fake_pid(rng),
                "name": f"Group {g}",
                "detail": {"devices": members},
            }
        )
    return devices, groups


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--devices", nargs="+", type=int, default=[25, 50, 76, 100, 200, 400])
    parser.add_argument("--write", metavar="DIR", help="Write payload files per size")
    args = parser.parse_args()

    print(f"{'devices':>8} {'groups':>7} {'dev_bytes':>10} {'grp_bytes':>10} {'total_KB':>9}")
    for n in args.devices:
        groups = max(1, n // 5)
        devices, grp = make_inventory(n, groups)
        dev_json = json.dumps({"abstract_devices": devices})
        grp_json = json.dumps(
            [
                {"group": {"avid": g["avid"], "name": g["name"], "devices": g["detail"]["devices"]}}
                for g in grp
            ]
        )
        total_kb = (len(dev_json) + len(grp_json)) / 1024
        print(f"{n:>8} {groups:>7} {len(dev_json):>10} {len(grp_json):>10} {total_kb:>9.1f}")
        if args.write:
            import os

            os.makedirs(args.write, exist_ok=True)
            with open(f"{args.write}/devices-{n}.json", "w") as f:
                f.write(dev_json)
            with open(f"{args.write}/groups-{n}.json", "w") as f:
                f.write(grp_json)


if __name__ == "__main__":
    main()
