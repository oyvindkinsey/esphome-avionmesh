# HTTP API & SSE

Base path: `/api/`
Web UI assets: `/ui` (HTML), `/ui.css`, `/ui.js`

## Thread Model

The HTTPD runs on a separate FreeRTOS task. HTTP handlers must not call mesh or MQTT APIs directly.

**Pattern:** handlers post a `DeferredAction` to a mutex-protected queue; `AvionMeshHub::loop()` drains it on the main ESPHome thread. Simple one-shot triggers use `volatile bool` flags to avoid allocation overhead.

## HTTP Endpoints

All endpoints: `POST`, `Content-Type: application/json`, except `/api/events`.

| Path | Action |
|------|--------|
| `GET /api/events` | SSE stream (max 2 concurrent sessions) |
| `POST /api/control` | Set brightness and/or color temp for a device/group |
| `POST /api/discover_mesh` | Trigger mesh ping scan |
| `POST /api/scan_unassociated` | Scan for unassociated BLE devices |
| `POST /api/claim_device` | Pair an unassociated device |
| `POST /api/add_discovered` | Add a device found via mesh ping scan |
| `POST /api/unclaim_device` | Remove a device |
| `POST /api/examine_device` | Query firmware/vendor info from a device |
| `POST /api/create_group` | Create a group |
| `POST /api/delete_group` | Delete a group |
| `POST /api/add_to_group` | Add a device to a group |
| `POST /api/remove_from_group` | Remove a device from a group |
| `POST /api/set_mqtt_exposed` | Enable/disable MQTT for a device, group, or broadcast (id=0) |
| `POST /api/set_min_brightness` | Set virtual-zero threshold for a device (`avion_id`, `min_brightness` 0–255) |
| `POST /api/save` | Persist DB to NVS |
| `POST /api/set_passphrase` | Set mesh passphrase |
| `POST /api/generate_passphrase` | Generate and store a new random passphrase |
| `POST /api/factory_reset` | Erase all data |
| `POST /api/import` | Import device/group/passphrase data (from `avion_import.py`) |

## SSE Events

Emitted to all connected clients. An initial sync is sent to each new client on connect.

| Event | Payload fields |
|-------|----------------|
| `meta` | `ble_state`, `rx_count` |
| `devices` | `devices[]` — each: `avion_id`, `name`, `product_type`, `product_name`, `groups`, `mqtt_exposed`, `has_dimming`, `has_color_temp`, `min_brightness`, optionally `brightness`, `color_temp` |
| `groups` | `groups[]` |
| `sync_complete` | _(none)_ |
| `device_added` | `avion_id`, `name`, `product_type`, `product_name`, `groups`, `mqtt_exposed`, `has_dimming`, `has_color_temp`, `min_brightness` |
| `device_removed` | `avion_id` |
| `group_added` | `group_id`, `name`, `members`, `mqtt_exposed` |
| `group_removed` | `group_id` |
| `group_updated` | `group_id`, `members` |
| `state` | `avion_id`, `brightness`, `color_temp` (optional) |
| `mesh_status` | `mesh_mqtt_exposed` |
| `discover_mesh` | `devices[]` — each: `device_id`, `fw`, `vendor_id`, `csr_product_id`, `known` |
| `scan_unassoc` | `uuid_hashes[]` |
| `claim_result` | `status`, `device_id` (on success), `message` (on failure) |
| `examine` | `avion_id`, `fw`, `vendor_id`, `csr_product_id`, `flags` — or `error` |
| `import_result` | `added_devices`, `added_groups` |
| `mqtt_toggled` | `id`, `mqtt_exposed` |
| `save_result` | _(none)_ |
| `debug` | string |
