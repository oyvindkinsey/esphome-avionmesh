#!/usr/bin/env python3
"""Dummy Avi-on cloud server for testing sync/import scaling.

Serves synthetic inventories (see cloud_scale.make_inventory) behind the
same endpoint shapes the real API uses, so import tooling and — once the
firmware offers a base-URL test override (requested on PR #26) — the
on-device sync can be driven against meshes of any size without touching
real account data.

Usage:
    tools/fake_cloud.py --devices 200 --groups 40 --port 8077
    AVION_API_BASE=http://127.0.0.1:8077/ tools/avion_import.py \\
        --email test@example.com --from-cloud-user ... (see --help)

Any email/password logs in and yields a fixed test token. Group detail
includes member device PIDs so membership resolution is exercised.
"""

import argparse
import json
from http.server import BaseHTTPRequestHandler, HTTPServer
from typing import ClassVar
from urllib.parse import urlparse

from cloud_scale import make_inventory

TOKEN = "fake-test-token"
LOCATION_PID = "f" * 24


class Handler(BaseHTTPRequestHandler):
    devices: ClassVar[list] = []
    groups: ClassVar[list] = []
    passphrase: ClassVar[str] = "AQEBAQEBAQEBAQEBAQEBAQ=="

    def log_message(self, *args):
        pass

    def _send(self, payload, code=200):
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _authorized(self):
        return self.headers.get("Authorization") == f"Token {TOKEN}"

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(length) or b"{}")
        path = urlparse(self.path).path.strip("/")
        if path == "sessions":
            if body.get("email") and body.get("password"):
                self._send({"credentials": {"auth_token": TOKEN}})
            else:
                self._send({"error": "missing credentials"}, 401)
        else:
            self._send({"error": "not found"}, 404)

    def do_GET(self):
        path = urlparse(self.path).path.strip("/")
        if path == "user/locations" and self._authorized():
            self._send({"locations": [{"pid": LOCATION_PID, "name": "Fake Home"}]})
        elif path == f"locations/{LOCATION_PID}" and self._authorized():
            self._send({"location": {"passphrase": self.passphrase}})
        elif path == f"locations/{LOCATION_PID}/abstract_devices" and self._authorized():
            self._send({"abstract_devices": self.devices})
        elif path == f"locations/{LOCATION_PID}/groups" and self._authorized():
            self._send({"groups": [{"avid": g["avid"], "pid": g["pid"]} for g in self.groups]})
        elif path.startswith("groups/") and self._authorized():
            pid = path.split("/", 1)[1]
            match = [g for g in self.groups if g["pid"] == pid]
            if not match:
                self._send({"error": "no such group"}, 404)
            else:
                g = match[0]
                self._send(
                    {
                        "group": {
                            "avid": g["avid"],
                            "name": g["name"],
                            "devices": g["detail"]["devices"],
                        }
                    }
                )
        else:
            self._send({"error": "unauthorized or unknown"}, 401 if not self._authorized() else 404)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--devices", type=int, default=76)
    parser.add_argument("--groups", type=int, default=16)
    parser.add_argument("--port", type=int, default=8077)
    args = parser.parse_args()

    devices, groups = make_inventory(args.devices, args.groups)
    Handler.devices = devices
    Handler.groups = groups
    print(f"Serving {len(devices)} devices, {len(groups)} groups on http://127.0.0.1:{args.port}/")
    HTTPServer(("127.0.0.1", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
