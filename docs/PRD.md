# ESPHome Avi-on Mesh — PRD

## Problem

Avi-on BLE mesh lights require Avi-on's proprietary cloud + mobile app. Cloud shutdown, API changes, or loss of internet = broken smart home.

## Solution

ESP32 gateway running ESPHome + this component. Speaks CSRMesh BLE to the lighting mesh; exposes devices to Home Assistant via MQTT auto-discovery. Fully local — no cloud required.

## Target Users

Home Assistant users with Avi-on BLE mesh lights who want local control and automation.

---

## Core Requirements

| # | Requirement |
|---|-------------|
| 1 | **BLE bridge** — connect to mesh via CSRMesh GATT; auto-reconnect on disconnect |
| 2 | **Device management** — claim/unclaim devices, create/delete groups, persist to NVS |
| 3 | **MQTT auto-discovery** — HA light entities per device/group (on/off, brightness, color temp) |
| 4 | **Silent color temp** — adjust warmth without changing on/off or brightness state |
| 5 | **Web UI** — discover, pair, group, and control devices from a browser |
| 6 | **Import tool** — `avion_import.py` migrates passphrase + devices from Avi-on cloud |
| 7 | **Group state inference** — when an external controller commands a group, infer and publish the group's MQTT state from the resulting device updates (CSRMesh RX carries no explicit group-command notification) |
| 8 | **Virtual zero (min brightness)** — per-device configurable floor value; non-zero BLE/MQTT brightness values below this threshold are clamped up to the threshold, supporting dimmers wired to loads that only respond above a minimum drive level |

## Key Design Decisions

- **Ethernet over Wi-Fi** — ESP32 shares radio between BLE and Wi-Fi; Ethernet keeps BLE exclusive
- **Opt-in MQTT** — devices not exported by default; enabled per-device/group via Web UI
- **Deferred action queue** — HTTP thread posts actions; main ESPHome loop consumes them (thread safety)
- **Single BLE bridge** — scan picks best-RSSI bridge; all mesh traffic routes through it

---

## Reference Docs

| Topic | Doc |
|-------|-----|
| BLE connection, mesh protocol, operations | [csrmesh.md](csrmesh.md) |
| Device/group data model, NVS format | [database.md](database.md) |
| MQTT topics, discovery, color temp | [mqtt.md](mqtt.md) |
| HTTP API endpoints, SSE events, thread model | [api.md](api.md) |
| Web UI requirements | [ui.md](ui.md) |
