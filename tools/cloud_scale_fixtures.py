#!/usr/bin/env python3
"""Deterministic synthetic inventories; byte counts are not ESP32 capacity claims."""
import argparse
import hashlib
import json
from pathlib import Path

from cloud_scale import make_inventory as upstream_make_inventory

SYNTHETIC_EMAIL = "scale@example.invalid"
SYNTHETIC_PASSWORD = "synthetic-scale-only"
SYNTHETIC_KEY = "AQEBAQEBAQEBAQEBAQEBAQ=="
LOCATION_PID = "f" * 24
TOKEN = "synthetic-scale-token"


def encode(payload):
    return json.dumps(payload, separators=(",", ":"), ensure_ascii=True).encode()


def make_inventory(n_devices, n_groups, seed=1, members=4, metadata_bytes=448):
    """Extend the unchanged upstream generator with sized metadata/memberships."""
    if not 1 <= n_devices <= 32768 or not 0 <= n_groups <= 24000:
        raise ValueError("devices must be 1..32768; groups 0..24000")
    if members < 0 or metadata_bytes < 0:
        raise ValueError("members and metadata_bytes must be nonnegative")
    devices, groups = upstream_make_inventory(n_devices, n_groups, seed)
    for device in devices:
        device["metadata"] = "s" * metadata_bytes
    # Keep the upstream four-member fixture exactly; deterministic wrapping
    # selects sparse/dense memberships without changing its PID/name generation.
    if members != 4:
        for index, group in enumerate(groups):
            group["detail"]["devices"] = [devices[(index + offset) % n_devices]["pid"]
                                                for offset in range(min(members, n_devices))]
    return devices, groups


def endpoints(devices, groups):
    data = {"sessions": {"credentials": {"auth_token": TOKEN}},
            "user/locations": {"locations": [{"pid": LOCATION_PID, "name": "Synthetic Home"}]},
            f"locations/{LOCATION_PID}": {"location": {"passphrase": SYNTHETIC_KEY}},
            f"locations/{LOCATION_PID}/abstract_devices": {"abstract_devices": devices},
            f"locations/{LOCATION_PID}/groups": {"groups": [{"avid": g["avid"], "pid": g["pid"]} for g in groups]}}
    for g in groups:
        data[f"groups/{g['pid']}"] = {"group": {"avid": g["avid"], "name": g["name"], "devices": g["detail"]["devices"]}}
    return data


def manifest(devices, groups):
    payloads = endpoints(devices, groups)
    sizes = {path: {"bytes": len(encode(body)), "sha256": hashlib.sha256(encode(body)).hexdigest()}
             for path, body in payloads.items()}
    return {"devices": len(devices), "groups": len(groups),
            "memberships": sum(len(g["detail"]["devices"]) for g in groups),
            "inventory_bytes": sum(value["bytes"] for path, value in sizes.items()
                                   if path.endswith(("abstract_devices", "/groups")) or path.startswith("groups/")),
            "endpoints": sizes}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--devices", nargs="+", type=int, default=[76, 200, 400, 512, 513])
    parser.add_argument("--groups", type=int, default=16)
    parser.add_argument("--members", type=int, default=4)
    parser.add_argument("--metadata-bytes", type=int, default=448)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--write", type=Path)
    args = parser.parse_args()
    reports = []
    for n in args.devices:
        devices, groups = make_inventory(n, args.groups, args.seed, args.members, args.metadata_bytes)
        reports.append(manifest(devices, groups))
        if args.write:
            folder = args.write / str(n)
            folder.mkdir(parents=True, exist_ok=True)
            for path, body in endpoints(devices, groups).items():
                (folder / (path.replace("/", "_") + ".json")).write_bytes(encode(body))
    print(json.dumps(reports, indent=2))


if __name__ == "__main__":
    main()
