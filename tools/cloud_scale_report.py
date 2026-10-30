#!/usr/bin/env python3
"""Run offline, certificate-verified synthetic HTTPS probes; collect ESP32 heap JSONL.

Host runs verify fixture/transport behavior only, not firmware parsing, heap, stack or NVS.
"""
import argparse
import contextlib
import hashlib
import http.client
import ipaddress
import json
import re
import ssl
import subprocess
import tempfile
import threading
from pathlib import Path

from cloud_scale_fixtures import (LOCATION_PID, SYNTHETIC_EMAIL, SYNTHETIC_PASSWORD, TOKEN,
                         encode)
from fake_cloud_tls import create_server

def make_cert(folder, host):
    """Short-lived self-signed test trust anchor with explicit SAN; never disable verification."""
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    cert, key = folder / "scale-cert.pem", folder / "scale-key.pem"
    if cert.exists() or key.exists():
        raise ValueError("refusing to overwrite certificate/key")
    try:
        ipaddress.ip_address(host)
        san = f"IP:{host}"
    except ValueError:
        if not re.fullmatch(r"[A-Za-z0-9.-]+", host):
            raise ValueError("invalid certificate host")
        san = f"DNS:{host}"
    config = folder / "openssl.cnf"
    config.write_text(f"[req]\nprompt=no\ndistinguished_name=dn\nx509_extensions=ext\n"
                      f"[dn]\nCN={host}\n[ext]\nsubjectAltName={san}\n"
                      "basicConstraints=critical,CA:TRUE\nkeyUsage=critical,digitalSignature,keyEncipherment,keyCertSign\n"
                      "extendedKeyUsage=serverAuth\n")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "7",
                    "-keyout", str(key), "-out", str(cert), "-config", str(config)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    key.chmod(0o600)
    return cert, key


@contextlib.contextmanager
def serving(cert, key, **kwargs):
    server = create_server(("127.0.0.1", 0), cert, key, **kwargs)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def request(server, context, path, login=False, timeout=2, credentials=None):
    connection = http.client.HTTPSConnection("127.0.0.1", server.server_port,
                                            context=context, timeout=timeout)
    headers = {"Authorization": f"Token {TOKEN}"}
    data = None
    if login:
        headers = {"Content-Type": "application/json"}
        data = encode(credentials or {"email": SYNTHETIC_EMAIL, "password": SYNTHETIC_PASSWORD})
    try:
        connection.request("POST" if login else "GET", "/" + path, data, headers)
        response = connection.getresponse()
        declared = response.getheader("Content-Length")
        if declared is not None:
            declared = int(declared)
            if declared < 0:
                raise ValueError("invalid_content_length")
        body = response.read(262145)
        if declared is not None and len(body) < min(declared, 262145):
            raise http.client.IncompleteRead(body, declared - len(body))
        if len(body) > 262144:
            raise ValueError("cloud_response_too_large")
        if response.status not in (200, 201):
            raise ValueError(f"http_{response.status}")
        return json.loads(body), {"path": path, "bytes": len(body), "sha256": hashlib.sha256(body).hexdigest()}
    finally:
        connection.close()


def sync_probe(server, context, existing=None):
    """Host shape/integrity oracle only; C++ validation remains separately tested."""
    trace = []
    def fetch(path, login=False):
        body, record = request(server, context, path, login)
        trace.append(record)
        return body
    assert fetch("sessions", True)["credentials"]["auth_token"] == TOKEN
    locations = fetch("user/locations")["locations"]
    assert len(locations) == 1
    pid = str(locations[0]["pid"])
    assert fetch(f"locations/{pid}")["location"]["passphrase"] == "AQEBAQEBAQEBAQEBAQEBAQ=="
    devices = fetch(f"locations/{pid}/abstract_devices")["abstract_devices"]
    groups = fetch(f"locations/{pid}/groups")["groups"]
    if len(devices) > 512 or len(groups) > 128:
        raise ValueError("configured_parser_cap")
    ids = {device["avid"] for device in devices}
    pids = {device["pid"] for device in devices}
    if len(ids) != len(devices):
        raise ValueError("duplicate_device_id")
    members = 0
    for group in groups:
        detail = fetch("groups/" + group["pid"])["group"]
        if not all(member in pids for member in detail["devices"]):
            raise ValueError("unresolved_group_member")
        members += len(detail["devices"])
    candidate = {"devices": ids, "groups": {group["avid"] for group in groups}}
    existing = existing or {"devices": set(), "groups": set()}
    return candidate, {"requests": trace, "devices": len(ids), "groups": len(groups), "memberships": members,
                       "added_devices": len(ids - existing["devices"]),
                       "added_groups": len(candidate["groups"] - existing["groups"])}


def offline_report(cert, key):
    context = ssl.create_default_context(cafile=str(cert))
    report = {"schema": 1, "scope": "host HTTPS and fixture checks only", "esp32_measurements": None,
              "scenarios": [], "failures": {}}
    for name, count, groups, members in [("minimum", 76, 16, 4), ("larger_sparse", 200, 40, 1),
                                         ("larger_dense", 200, 40, 200), ("parser_boundary", 512, 128, 1)]:
        # Isolate the parser count boundary from the independent response byte cap.
        metadata_bytes = 0 if name == "parser_boundary" else 448
        with serving(cert, key, devices=count, groups=groups, members=members,
                     metadata_bytes=metadata_bytes) as server:
            existing = None
            runs = []
            for _ in range(3):
                existing, result = sync_probe(server, context, existing)
                runs.append(result)
            assert runs[1]["added_devices"] == runs[1]["added_groups"] == 0
            report["scenarios"].append({"name": name, "fixture": server.inventory_manifest, "runs": runs})
    for fault, expected in [("unauthorized", "http_401"), ("redirect", "http_302"),
                            ("malformed", "JSONDecodeError"), ("oversize", "cloud_response_too_large"),
                            ("disconnect", "IncompleteRead"), ("slow", "TimeoutError"),
                            ("duplicate-id", "duplicate_device_id"), ("unresolved-member", "unresolved_group_member")]:
        with serving(cert, key, fault=fault, slow_seconds=2.2) as server:
            try:
                sync_probe(server, context)
            except (ValueError, http.client.IncompleteRead, TimeoutError) as error:
                actual = str(error) if isinstance(error, ValueError) and not isinstance(error, json.JSONDecodeError) else type(error).__name__
                assert actual == expected, (fault, actual, expected)
                report["failures"][fault] = actual
            else:
                raise AssertionError(f"fault {fault} unexpectedly succeeded")
    with serving(cert, key, devices=513, metadata_bytes=0) as server:
        try:
            sync_probe(server, context)
        except ValueError as error:
            assert str(error) == "configured_parser_cap"
            report["failures"]["parser_cap"] = str(error)
        else:
            raise AssertionError("parser cap not rejected")
    with serving(cert, key) as server:
        for name, probe_context in [("untrusted_ca", ssl.create_default_context()),
                                    ("hostname_mismatch", ssl.create_default_context(cafile=str(cert)))]:
            if name == "hostname_mismatch":
                # SAN has only 127.0.0.1, so localhost must fail even though it resolves locally.
                conn = http.client.HTTPSConnection("localhost", server.server_port, context=probe_context)
                try:
                    conn.request("GET", "/user/locations")
                except ssl.SSLCertVerificationError:
                    report["failures"][name] = "SSLCertVerificationError"
                else:
                    raise AssertionError(name)
                finally:
                    conn.close()
            else:
                try:
                    request(server, probe_context, "sessions", True)
                except ssl.SSLCertVerificationError:
                    report["failures"][name] = "SSLCertVerificationError"
                else:
                    raise AssertionError(name)
        try:
            request(server, context, "sessions", True, credentials={"email": "real@example.com", "password": "not-accepted"})
        except ValueError as error:
            assert str(error) == "http_401"
            report["failures"]["nonsynthetic_login"] = str(error)
    return report


def collect_logs(path):
    samples = []
    for line in Path(path).read_text(errors="replace").splitlines():
        marker = 'CLOUD_SCALE {'
        if marker in line:
            # ESPHome may append an ANSI color reset after the JSON object.
            payload = line[line.index(marker) + len('CLOUD_SCALE '):]
            samples.append(json.JSONDecoder().raw_decode(payload)[0])
    if not samples:
        raise ValueError("no CLOUD_SCALE JSON samples found")
    return {"schema": 1, "scope": "measured ESP32 log samples", "samples": samples}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--make-cert", type=Path)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--logs", type=Path, help="Collect captured serial/log output; no device connection")
    args = parser.parse_args()
    if args.make_cert:
        cert, key = make_cert(args.make_cert, args.host)
        print(json.dumps({"cert": str(cert), "key": str(key)}))
        return
    if args.logs:
        report = collect_logs(args.logs)
    else:
        with tempfile.TemporaryDirectory(prefix="avion-scale-") as folder:
            cert, key = make_cert(folder, "127.0.0.1")
            report = offline_report(cert, key)
    data = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(data)
        print(f"Wrote {args.output}; {report['scope']}")
    else:
        print(data, end="")


if __name__ == "__main__":
    main()
