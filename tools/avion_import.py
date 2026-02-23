#!/usr/bin/env python3
"""Download devices/groups from Avi-on cloud and import into an ESPHome avionmesh device."""

import argparse
import json
import sys
from urllib.request import Request, urlopen
from urllib.error import HTTPError, URLError

API = "https://api.avi-on.com/"


def api_request(path, body=None, auth_token=None):
    url = API + path
    headers = {"Content-Type": "application/json"}
    if auth_token:
        headers["Accept"] = "application/api.avi-on.v3"
        headers["Authorization"] = f"Token {auth_token}"

    data = json.dumps(body).encode() if body else None
    method = "POST" if data else "GET"
    req = Request(url, data=data, headers=headers, method=method)

    try:
        with urlopen(req, timeout=30) as resp:
            return json.loads(resp.read())
    except HTTPError as e:
        body = e.read().decode(errors="replace")
        print(f"HTTP {e.code} from {url}: {body}", file=sys.stderr)
        raise SystemExit(1)
    except URLError as e:
        print(f"Connection failed: {e.reason}", file=sys.stderr)
        raise SystemExit(1)


def login(email, password):
    print(f"Logging in as {email}...")
    resp = api_request("sessions", {"email": email, "password": password})
    if "credentials" not in resp:
        print("Login failed — check email/password", file=sys.stderr)
        raise SystemExit(1)
    token = resp["credentials"]["auth_token"]
    print("  Authenticated")
    return token


def fetch_location(token):
    resp = api_request("user/locations", auth_token=token)
    locations = resp.get("locations", [])
    if not locations:
        print("No locations found in account", file=sys.stderr)
        raise SystemExit(1)

    loc = locations[0]
    loc_pid = loc["pid"]
    print(f"  Location: {loc.get('name', loc_pid)} (pid={loc_pid})")

    detail = api_request(f"locations/{loc_pid}", auth_token=token)
    passphrase = detail["location"]["passphrase"]
    print(f"  Passphrase: {'found' if passphrase else 'NOT FOUND'}")
    return loc_pid, passphrase, token


def fetch_devices(token, location_pid):
    print("Fetching devices...")
    resp = api_request(f"locations/{location_pid}/abstract_devices", auth_token=token)
    devices = []
    pid_to_avid = {}
    for raw in resp.get("abstract_devices", []):
        if raw.get("type") != "device":
            continue
        avid = raw.get("avid", 0)
        if avid == 0:
            continue
        pid_to_avid[raw["pid"]] = avid
        devices.append({
            "device_id": avid,
            "name": raw.get("name", "Device"),
            "product_type": raw.get("product_id", 134),
        })
    print(f"  {len(devices)} device(s)")
    return devices, pid_to_avid


def fetch_groups(token, location_pid, pid_to_avid):
    print("Fetching groups...")
    resp = api_request(f"locations/{location_pid}/groups", auth_token=token)
    groups = []
    for raw in resp.get("groups", []):
        avid = raw.get("avid", 0)
        if avid == 0:
            continue
        pid = raw["pid"]
        detail = api_request(f"groups/{pid}", auth_token=token)
        raw_members = detail.get("group", {}).get("devices", [])
        # API returns device PIDs (string MongoDB ObjectIDs) — resolve to avion_ids
        members = []
        for m in raw_members:
            if isinstance(m, str):
                mid = pid_to_avid.get(m, 0)
            elif isinstance(m, int):
                mid = m
            elif isinstance(m, dict):
                mid = m.get("avid") or m.get("avion_id") or m.get("device_id") or m.get("id", 0)
            else:
                continue
            if mid:
                members.append(mid)

        groups.append({
            "group_id": avid,
            "name": raw.get("name", "Group"),
            "members": members,
        })
        print(f"  Group '{raw.get('name')}' (avid={avid}): {len(members)} member(s)")
    print(f"  {len(groups)} group(s) total")
    return groups


def send_import(device_ip, payload, username=None, password_http=None):
    url = f"http://{device_ip}/api/import"
    data = json.dumps(payload).encode()
    headers = {"Content-Type": "application/json"}
    if username and password_http:
        import base64
        cred = base64.b64encode(f"{username}:{password_http}".encode()).decode()
        headers["Authorization"] = f"Basic {cred}"
    req = Request(url, data=data, headers=headers, method="POST")

    print(f"\nImporting to {device_ip}...")
    print(f"  Payload: {len(data)} bytes, {len(payload.get('devices', []))} devices, "
          f"{len(payload.get('groups', []))} groups")

    try:
        with urlopen(req, timeout=30) as resp:
            result = json.loads(resp.read())
            print(f"  Response: {result}")
    except HTTPError as e:
        body = e.read().decode(errors="replace")
        print(f"Import failed — HTTP {e.code}: {body}", file=sys.stderr)
        raise SystemExit(1)
    except URLError as e:
        print(f"Cannot reach device at {device_ip}: {e.reason}", file=sys.stderr)
        raise SystemExit(1)


def load_from_file(path):
    """Load mesh_db.json produced by mesh-tui and convert to ESPHome import payload."""
    with open(path) as f:
        db = json.load(f)

    # Reconstruct group members from per-device 'groups' lists
    group_members = {}
    for dev in db.get("devices", []):
        for gid in dev.get("groups", []):
            group_members.setdefault(gid, []).append(dev["device_id"])

    devices = [
        {"device_id": d["device_id"], "name": d["name"], "product_type": d.get("product_id", 0)}
        for d in db.get("devices", [])
    ]
    groups = [
        {"group_id": g["group_id"], "name": g["name"], "members": group_members.get(g["group_id"], [])}
        for g in db.get("groups", [])
    ]
    payload = {"devices": devices, "groups": groups}
    if db.get("passphrase"):
        payload["passphrase"] = db["passphrase"]
    return payload


def main():
    parser = argparse.ArgumentParser(description="Import Avi-on cloud data into ESPHome avionmesh device")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--from-file", metavar="PATH", help="Import from a local mesh_db.json (produced by mesh-tui)")
    source.add_argument("--email", metavar="EMAIL", help="Avi-on account email (cloud import)")
    parser.add_argument("--password", help="Avi-on account password (required with --email)")
    parser.add_argument("--device", required=True, help="ESPHome device IP address or hostname")
    parser.add_argument("--dry-run", action="store_true", help="Show payload but don't send to device")
    parser.add_argument("--no-reset", action="store_true", help="Don't clear existing data before import")
    parser.add_argument("--username", help="HTTP Basic Auth username for the ESPHome device")
    parser.add_argument("--password-http", help="HTTP Basic Auth password for the ESPHome device")
    args = parser.parse_args()

    if args.from_file:
        payload = load_from_file(args.from_file)
    else:
        if not args.password:
            parser.error("--password is required with --email")
        token = login(args.email, args.password)
        location_pid, passphrase, token = fetch_location(token)
        devices, pid_to_avid = fetch_devices(token, location_pid)
        groups = fetch_groups(token, location_pid, pid_to_avid)
        payload = {"devices": devices, "groups": groups}
        if passphrase:
            payload["passphrase"] = passphrase

    if not args.no_reset:
        payload["reset"] = True

    if args.dry_run:
        print("\n--- Dry run (would send): ---")
        print(json.dumps(payload, indent=2))
    else:
        send_import(args.device, payload, args.username, args.password_http)
        print("Done!")


if __name__ == "__main__":
    main()
