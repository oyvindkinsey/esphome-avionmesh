#!/usr/bin/env python3
"""Synthetic-only Avi-on HTTPS server. Requires a certificate and private key.

Bind --bind 0.0.0.0 explicitly for a staged ESP32 LAN test. Never use real credentials.
Generate a short-lived SAN certificate with cloud_scale_report.py --make-cert DIR --host LAN_IP.
"""
import argparse
import json
import socket
import ssl
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

from cloud_scale_fixtures import (LOCATION_PID, SYNTHETIC_EMAIL, SYNTHETIC_PASSWORD, TOKEN,
                         encode, endpoints, make_inventory, manifest)

FAULTS = ["none", "unauthorized", "redirect", "malformed", "oversize", "disconnect", "slow",
          "unresolved-member", "duplicate-id"]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass  # Credentials and tokens never logged.

    def _send(self, body, code=200, headers=None):
        if not isinstance(body, bytes):
            body = encode(body)
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.end_headers()
        try:
            for pos in range(0, len(body), 1024):
                self.wfile.write(body[pos:pos + 1024])
                if self.server.chunk_delay:
                    self.wfile.flush()
                    time.sleep(self.server.chunk_delay)
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass  # Expected when a size/deadline test closes the connection.

    def _route(self, login=False):
        path = urlparse(self.path).path.strip("/")
        if login:
            try:
                size = int(self.headers.get("Content-Length", "0"))
                if not 0 < size <= 1024:
                    return self._send({"error": "invalid login body"}, 400)
                body = json.loads(self.rfile.read(size))
            except (ValueError, json.JSONDecodeError):
                return self._send({"error": "invalid login body"}, 400)
            if path != "sessions" or body != {"email": SYNTHETIC_EMAIL, "password": SYNTHETIC_PASSWORD}:
                return self._send({"error": "synthetic credentials required"}, 401)
        elif self.headers.get("Authorization") != f"Token {TOKEN}":
            return self._send({"error": "unauthorized"}, 401)
        if path == self.server.fault_path:
            fault = self.server.fault
            if fault == "unauthorized":
                return self._send({"error": "injected"}, 401)
            if fault == "redirect":
                return self._send({}, 302, {"Location": "https://redirect.invalid/must-not-follow"})
            if fault == "malformed":
                return self._send(b'{"broken":')
            if fault == "oversize":
                return self._send(b" " * 262145)
            if fault == "disconnect":
                self.send_response(200)
                self.send_header("Content-Length", "100")
                self.end_headers()
                self.wfile.write(b"{")
                self.wfile.flush()
                self.connection.shutdown(socket.SHUT_RDWR)
                self.connection.close()
                return
            if fault == "slow":
                time.sleep(self.server.slow_seconds)
        payload = self.server.payloads.get(path)
        if payload is None or (not login and path == "sessions"):
            return self._send({"error": "not found"}, 404)
        return self._send(payload)

    def do_POST(self):
        self._route(login=True)

    def do_GET(self):
        self._route()


def create_server(address, cert, key, devices=76, groups=16, members=4, metadata_bytes=448,
                  seed=1, fault="none", fault_path=None, slow_seconds=16, chunk_delay=0):
    devs, grps = make_inventory(devices, groups, seed, members, metadata_bytes)
    if fault == "duplicate-id" and len(devs) > 1:
        devs[-1]["avid"] = devs[0]["avid"]
    if fault == "unresolved-member" and grps:
        grps[0]["detail"]["devices"] = ["unknown-synthetic-pid"]
    server = ThreadingHTTPServer(address, Handler)
    server.daemon_threads = True
    server.payloads = endpoints(devs, grps)
    server.inventory_manifest = manifest(devs, grps)
    server.fault, server.fault_path = fault, fault_path or f"locations/{LOCATION_PID}/abstract_devices"
    server.slow_seconds, server.chunk_delay = slow_seconds, chunk_delay
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(cert, key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    return server


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cert", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8443)
    parser.add_argument("--devices", type=int, default=76)
    parser.add_argument("--groups", type=int, default=16)
    parser.add_argument("--members", type=int, default=4)
    parser.add_argument("--metadata-bytes", type=int, default=448)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--fault", choices=FAULTS, default="none")
    parser.add_argument("--fault-path")
    parser.add_argument("--slow-seconds", type=float, default=16)
    parser.add_argument("--chunk-delay", type=float, default=0)
    args = parser.parse_args()
    server = create_server((args.bind, args.port), args.cert, args.key, args.devices, args.groups,
                           args.members, args.metadata_bytes, args.seed, args.fault, args.fault_path,
                           args.slow_seconds, args.chunk_delay)
    print(json.dumps({"listen": [args.bind, server.server_port], "tls": True,
                      "synthetic_only": True, "fixture": server.inventory_manifest}, indent=2), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
