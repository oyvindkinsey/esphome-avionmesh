# Synthetic HTTPS sync scaling (staged)

Use `tools/avion_import.py` as the primary import path, especially for larger
inventories. On-device sync is a convenience whose ESP32 operating envelope is
still unmeasured. The 256 KiB response, 512-device and 128-group parser caps are
rejection bounds, not proven capacities. If measured memory pressure appears,
prefer streaming/filtering or verified API pagination over raising caps; the
complete retained inventory and merge candidates still scale with membership.

Everything below uses synthetic fixtures. No physical test has been performed
with this harness. Host byte counts and host HTTPS checks cannot establish ESP32
TLS heap, stack headroom, persistence behavior or BLE/MQTT responsiveness.

## Host tools

```
python3 tools/cloud_scale_report.py --output /tmp/cloud-scale-report.json
python3 tools/cloud_scale_fixtures.py --devices 76 200 400 512 513 --groups 16 --write /tmp/scale-fixtures
```

Upstream `tools/fake_cloud.py` (plaintext, loopback) and `tools/cloud_scale.py`
remain unchanged for existing CLI workflows. The new TLS harness reuses the
upstream three-argument generator and adds ignored metadata/membership shapes.

The report runner generates a temporary seven-day self-signed certificate with an
IP SAN, explicitly trusts it, then serves only loopback HTTPS. It checks the real
endpoint shapes, minimum 76/16 fixture (approximately 45 KiB including ignored
metadata), larger sparse/dense memberships, three repeat runs with a populated
host candidate, parser-count boundary/rejection, oversize response, authentication,
redirect refusal, malformed/truncated JSON, slow response and unresolved members.
It also requires untrusted-CA and wrong-hostname failures. The host candidate
oracle is not the firmware C++ parser or NVS implementation.

For a staged LAN ESP32 run, replace `192.0.2.10` with the test machine's reachable
LAN IP (the SAN must match exactly), then generate a new certificate:

```
python3 tools/cloud_scale_report.py --make-cert /tmp/scale-tls --host 192.0.2.10
python3 tools/fake_cloud_tls.py --bind 0.0.0.0 --port 8443 \
  --cert /tmp/scale-tls/scale-cert.pem --key /tmp/scale-tls/scale-key.pem \
  --devices 76 --groups 16 --members 4
```

The private key stays on the test machine. Firmware receives only the public
certificate. Keep the host firewall scoped to the test LAN. Binding is loopback
by default; there is no plaintext mode. Login accepts only
`scale@example.invalid` / `synthetic-scale-only`; the mesh key is synthetic.
Choose faults with `--fault unauthorized|redirect|malformed|oversize|disconnect|slow|unresolved-member|duplicate-id`.
The default fault endpoint is abstract_devices; `--fault-path` selects another
path. `--slow-seconds 16` exceeds the firmware's request timeout; `--chunk-delay`
can expose transient allocations. Restart the server between cases using the
same seed and fixture shape.

## One basement session checklist

Record firmware revision, board, ESPHome/IDF versions, PSRAM build configuration,
TLS cipher, server fixture manifest (per-endpoint bytes and SHA-256), boot log and
network conditions. Start with 76 devices / 16 groups, then 200/40 sparse and dense,
then larger sizes only while measured headroom remains acceptable. Repeat each
successful case at least three times in one boot to measure fragmentation and
already populated merges. Keep BLE, MQTT and UI active; record continuity and
watchdog/reset evidence separately from heap logs.

Exercise wrong login, CA/hostname rejection, redirect, malformed/truncated/oversize
response, response deadline, invalid references and configured parser caps. Verify
a failed job releases its busy state and a later valid job succeeds. Explicit
allocation-failure injection is not implemented; do not label a size rejection
as an allocation-failure test. The firmware's nonthrowing allocation-failure
coverage is limited to HTTP-client and task creation; C++/ArduinoJson allocation
exhaustion requires a separate controlled run on the test build.

Collect logs offline:

```
python3 tools/cloud_scale_report.py --logs /path/to/captured.log --output /tmp/esp32-scale.json
```

Do not infer measured device capacity from a passing host report. Define any
measured envelope using devices, groups, membership density, current DB size,
board/PSRAM configuration and simultaneous controller load. Save-phase and NVS
watchdog evidence require an isolated persistence run; a dry-run cannot supply it.

## Experimental firmware provider

Add this block only to a dedicated test profile (replace the example IP and file
path). Normal firmware has no endpoint selector; the production fixed host,
public certificate bundle and disabled redirects remain unchanged.

```yaml
avionmesh:
  experimental_cloud_scale:
    url: https://192.0.2.10:8443/
    ca_certificate: /tmp/scale-tls/scale-cert.pem
```

This adds `AVIONMESH_CLOUD_SCALE_TEST` at compile time. The worker uses the pinned
public test CA, validates hostname and validity dates, requires synchronized time,
and still refuses redirects. Test builds reject every login except the fixed
synthetic credentials, and compare only the fixed synthetic mesh key. No home key
or real credential can be transmitted to the selected test provider.

Trigger the ordinary sync UI/API with those synthetic credentials. All candidates
merge into separate in-RAM synthetic vectors retained until reboot. The passed
home DeviceDB is never modified, and loop() returns false to suppress hub discovery
and cache invalidation for synthetic results. Status includes `experimental_synthetic`
and `dry_run`; successful runs still report added counts. No automatic startup job
or hardware action is added. Repeat without reboot to exercise a populated candidate.

Each `CLOUD_SCALE` log is JSON schema 1 with monotonic `us`, `phase`, `task`, byte
counts for internal/PSRAM free/largest blocks, global minimum free since boot and
ESP-IDF task stack high-water mark **in bytes**. PSRAM fields are zero when absent.
Receive before/after append, parse while raw body overlaps JsonDocument,
accumulation, merge-copy and save boundaries supplement a 50ms worker sampler.
A global boot watermark includes earlier activity; it is not a job-local minimum.
The worker sampler reports worker stack; main-loop phase samples identify that
task separately. Logging/sampling adds memory and timing overhead. The dry-run
emits `save_skipped_dry_run`, which must never be reported as measured NVS saving.

For the save phase, explicitly add `persist: true` beneath
`experimental_cloud_scale`. Default `persist: false` never writes NVS. The compile
guard `AVIONMESH_CLOUD_SCALE_PERSIST` instead constructs a dedicated DeviceDB whose
fixed namespace is `avion_scale`; load, clear and the candidate used by checked
save all preserve that namespace. It is not a user-supplied namespace. This mode
uses the actual DeviceDB serializer and checked NVS save path, emits
`save_test_namespace_before/after/failed`, and retains synthetic metadata over
reboot. No synthetic metadata is handed to the hub, MQTT discovery or mesh.
The namespace shares the physical NVS partition, so its flash space and allocator
pressure are part of the measured test. Do not claim this is identical to the
home namespace's free-space/fragmentation history. Use a spare test profile/device
for deliberate full-partition/exhaustion runs. Returning to a normal build leaves
`avion_scale` unused; it does not migrate it into the home namespace.

The host `test_cloud_scale_nvs` executable drives the real DeviceDB implementation
through fake NVS functions and verifies that candidate save, reload, clear and
failed commit never open the home namespace. It also verifies that failed save
keeps the in-RAM candidate unchanged. This is isolation coverage, not a hardware
NVS/watchdog or flash atomicity test. In the basement session capture the actual
save boundary metrics, watchdog responsiveness and restart/reload result with
`persist: true`, alongside a snapshot of the home inventory before and after.

```
clang++ -std=c++20 -DUSE_ESP32 -DAVIONMESH_CLOUD_SCALE_TEST \
  -Itests/scale_nvs_stubs -Itests/stubs -Icomponents/avionmesh \
  tests/test_cloud_scale_nvs.cpp components/avionmesh/device_db.cpp \
  -o /tmp/test_cloud_scale_nvs
/tmp/test_cloud_scale_nvs
python3 tests/test_cloud_scale_tools.py
```

## HTTP response bounds and deadline behavior

The response-body limit is 262144 bytes. A chunk crossing it is rejected before
append, failure stays sticky, and the first failure calls
`esp_http_client_close()` to close the socket. Callback return values alone are
not an abort mechanism in ESP-IDF 5.5.6: its `http_on_body()` ignores the event
handler return. The close guard is set before calling close because the SDK
synchronously emits DISCONNECTED. Terminal, error and finish callbacks never
initiate another close. Client/parser cleanup occurs only after perform returns;
`cancel_request` is not used because that API closes and reconnects.

The three-minute job budget starts after the worker takes its credentials, before
the clock wait. Initial socket timeout is at most the remaining budget or 15s.
CONNECTED, HEADERS_SENT, completed-header and body-data events refresh that limit;
the first such event at/after the deadline closes the socket. After perform and
cleanup, the component checks its sticky failure flags and deadline again and
requires complete HTTP framing before any JSON parsing or inventory merge. A
syntactically valid JSON prefix from a truncated HTTP response cannot complete a
job. Expiry is reported as `sync_timeout`; the size limit reports
`cloud_response_too_large`.

This is a checked job deadline, not a guaranteed wall-clock cancellation timer.
A blocking SDK/DNS/TLS operation may return later than its requested timeout, and
a server slowly trickling an unfinished HTTP header can avoid completed-header
callbacks while resetting an inactivity timeout. This callback-based client
cannot interrupt that phase at an absolute instant. It still refuses completion
after the deadline. A strict all-phase wall-clock bound requires a separate
transport/cancellation design; do not label the current deadline test as proving
one. The synthetic data-trickle test proves active close when body callbacks
continue past the budget, while the normal slow-server case proves inactivity
timeout behavior. Hardware timing remains unmeasured.

`test_cloud_http_bounds` is an SDK-independent policy fixture whose transport
intentionally ignores callback return values. It exercises sticky size/deadline
failure, exact cap and cap-plus-one, no appends after failure, one active close,
synchronous terminal-event reentry, decreasing remaining-budget timeouts, body
trickle expiry, and final rejection of truncated or late responses. Run offline:

```sh
clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Icomponents/avionmesh tests/test_cloud_http_bounds.cpp \
  -o /tmp/cloud-http-bounds-test
/tmp/cloud-http-bounds-test
```
