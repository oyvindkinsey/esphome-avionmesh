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
| `GET /api/events` | SSE stream (max 3 concurrent sessions; extra clients receive HTTP 503) |
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
| `GET /api/export` | Backup in the same schema `POST /api/import` accepts (`reset`, `passphrase`, `devices`, `groups`). The passphrase is always included — a backup without the key cannot restore the mesh, so store the file safely |
| `POST /api/rename_device` | Rename a device (`avion_id`, `name`); republishes MQTT discovery, emits `device_updated` |
| `POST /api/rename_group` | Rename a group (`group_id`, `name`); republishes MQTT discovery, emits `group_updated` |

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
| `discover_mesh` | `devices[]` — each: `device_id`, `fw`, `vendor_id`, `csr_product_id`, `known`, `short` (true when the ping reply was too short to carry fw/vendor/product fields; the other fields are then zero) |
| `scan_unassoc` | `devices[]` — each: `uuid_hash`, `vendor_code`, `product_code` (decoded from the UUID announce; the Web UI preselects the device type from these) |
| `claim_result` | `status`, `device_id` (on success), `message` (on failure) |
| `examine` | `avion_id`, `fw`, `vendor_id`, `csr_product_id`, `flags` — or `error` |
| `import_result` | `added_devices`, `added_groups` |
| `mqtt_toggled` | `id`, `mqtt_exposed` |
| `save_result` | _(none)_ |
| `debug` | string |


## SSE delivery and connection lifetime

The component keeps at most three SSE sessions, including closing sessions until
HTTPD releases their contexts. A fourth connection receives HTTP 503; opening a
new view does not delete an existing monitor or leave its socket silently idle.
The existing browser EventSource retry behavior can reconnect after capacity is
available. The endpoint and event schemas are unchanged.

Each session has a FIFO bounded by 16 frames and 8192 total retained wire bytes.
Partial writes and temporary EAGAIN retain the current frame and later events in
order. Fatal send errors, persistent timeout attempts, an oversized individual
frame, or queue overflow request asynchronous socket close. A successful close
request is queued once; a full/stopping HTTPD work mailbox may fail and is retried.
No event is silently skipped while the connection is advertised as healthy.
Overflow is an explicit disconnect/resync policy, not durable event delivery.
Clients that need a complete current inventory must reconnect and receive a new
snapshot after disconnect; transient physical command events are never replayed.

Initial sync is paced: at most one frame is added per main-loop turn, and its
cursor waits for pending output to drain. Devices and groups each use one
record per frame so escaped names and dense membership arrays do not make a
multi-record batch exceed the same byte bound. The existing UI merges each
`groups` batch by group ID. A cursor advances only after enqueue succeeds, and `sync_complete` follows every accepted snapshot
frame in the same FIFO. An oversized single record closes the stream instead of
skipping it or claiming snapshot completion. A snapshot is assembled over time,
with the existing live update events interleaved; it is not a transactional copy
of the entire database.

Main-loop snapshot/event producers, connection admission and queue draining share
a manager mutex. Socket sends, session lookup and close requests run on HTTPD's
own task through `httpd_queue_work`, serialized with IDF socket/context cleanup.
`CONFIG_HTTPD_QUEUE_WORK_BLOCKING` is disabled: a full work mailbox must fail for
retry rather than deadlock the HTTPD task waiting on itself. The API and setting
were checked in the cached IDF 5.5.5 and 5.5.6 sources. Queue submissions use the
permanent component handler as their argument; no per-work heap context is leaked
when the server stops before executing queued work.

IDF's `sess_ctx` owns a separate shared reference to each session. Its `free_ctx`
callback releases that reference and atomically signals cleanup. Manager removal,
server-stop cleanup, or a failed close-queue attempt cannot destroy an object to
which IDF still holds a reference. The handler has the normal firmware-long
component lifetime; this does not add dynamic component/server destruction APIs.

The byte cap is a wire-payload bound, not an ESP32 heap measurement. Strings,
FIFO nodes, shared-reference control blocks, the session/context objects and
allocator bookkeeping add overhead. Frame construction and initial-sync JSON
serialization also allocate temporary strings outside the retained byte count.
Buffers allocate on demand; three active sessions can retain at most 24 KiB of
wire frames plus that overhead. Before/after heap measurements remain a separate
hardware exercise.

`test_sse_sessions` compiles the same production queue, lifetime, scheduler and
paced serializer code with fake transport operations. It checks partial writes,
EAGAIN and ordered waiting/reading/saved delivery; fatal errors and close retries;
byte/frame overflow; reject-new capacity; delayed free-context and server-stop
ownership; work-mailbox retry; concurrent admission/broadcast/drain/cleanup; and
complete 19-device/10-group, 81/16, and dense 200/40 snapshots (8000 memberships).
Address/undefined-behavior and thread sanitizers are suitable offline checks;
these fixtures do not claim device/socket timing or live browser validation.
