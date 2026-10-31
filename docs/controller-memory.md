# Read-only controller memory diagnostics

The existing management request `{"action":"controller_status"}` now adds
`memory` and `controller_storage`. The request sends no mesh command, does not
save NVS, drain queues, clear snapshots/counters, expire a capture, reset the
gateway, or reset heap low-water marks. Existing event counters and
`local_dimmers` remain available. This instrumentation is preparation for a
controlled hardware measurement, not evidence that such a measurement passed.

The heap sample is taken before allocating/serializing the status response.
Request parsing, the running application and prior responses can still affect
the allocator. ESP-IDF samples each metric separately: another task may allocate
between calls, so this is not one atomic snapshot of every heap region.

## Exact response fields

| Path | Meaning |
| --- | --- |
| `memory.schema_version` | `1` |
| `memory.units` | `"bytes"` for all memory/stack metrics |
| `memory.platform` | `"esp_idf"` on ESP32, `"unsupported"` on the host |
| `memory.sampled_at_ms` | Gateway `millis()` at sampling; boot-relative wrapping 32-bit milliseconds, not UTC |
| `memory.sample_scope` | `"before_status_response_serialization"` |
| `memory.minimum_scope` | `"boot_lifetime_per_region_sum"`; see the caveats below |
| `memory.internal.capabilities` | `"internal_8bit"`: `MALLOC_CAP_INTERNAL \| MALLOC_CAP_8BIT` |
| `memory.psram.capabilities` | `"spiram_8bit"`: `MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT` |
| `memory.{internal,psram}.available` | Whether the platform exposes a matching nonempty heap pool |
| `memory.{internal,psram}.unavailable_reason` | `null`, `"unsupported_platform"`, or `"no_matching_heap"` |
| `memory.{internal,psram}.total_bytes` | `heap_caps_get_total_size(caps)` |
| `memory.{internal,psram}.free_bytes` | Current `heap_caps_get_free_size(caps)` |
| `memory.{internal,psram}.minimum_free_bytes` | `heap_caps_get_minimum_free_size(caps)` without starting/resetting local monitoring |
| `memory.{internal,psram}.largest_free_block_bytes` | `heap_caps_get_largest_free_block(caps)`, distinct from total free bytes |
| `memory.task.available` | Calling-task information available on ESP-IDF |
| `memory.task.context` | `"controller_status_calling_task"`; do not assume it is the BLE, worker or main task |
| `memory.task.name` | `pcTaskGetName(nullptr)`, or `null` when unavailable |
| `memory.task.stack_minimum_available` | Whether `INCLUDE_uxTaskGetStackHighWaterMark` enables the stack metric |
| `memory.task.stack_minimum_free_bytes` | Calling task's `uxTaskGetStackHighWaterMark(nullptr)`, or `null`; ESP-IDF returns **bytes**, unlike upstream FreeRTOS words |
| `memory.task.stack_minimum_scope` | `"task_lifetime"`, since this calling task started |

When a pool is unavailable, all four numeric pool fields are `null`, not zero.
A genuine measured zero remains numeric zero with `available: true`. On a host,
both heap pools and task/stack metrics are explicitly unavailable; a machine's
host RAM is not substituted for ESP32 measurements.

ESP-IDF's minimum query sums the allocator regions' lifetime low-water marks.
Different regions may reach minima at different times, so the sum can be lower
than the smallest simultaneously available total. The API excludes regions
added at runtime from its minimum calculation. Preserve these caveats when
interpreting PSRAM or comparing total/free/minimum values. This code never
starts a phase-local minimum monitor. If another experimental component changes
the allocator's monitoring scope, record that separately rather than labelling
the result as a clean boot-lifetime comparison.

`controller_storage` describes retained controller state without pretending to
measure allocator overhead:

| Path under `controller_storage` | Meaning |
| --- | --- |
| `inventory_devices`, `inventory_groups`, `inventory_controllers` | Current saved inventory counts; controllers are products 81/91/126/127 |
| `map_entries.absolute_dimmer_streams`, `map_entries.local_dimmer_streams` | Retained per-source direction/local-report map entries |
| `map_entries.slot_read_controllers` | Retained controller entries in the slot-read cache, including incomplete entries |
| `map_entries.binding_refresh_statuses`, `map_entries.keypad_setup_statuses` | Retained status-map entries |
| `snapshots.bindings_retained`, `snapshots.bindings_valid` | Retained binding snapshots, including invalid snapshots, and valid subset |
| `snapshots.binding_vector_capacity` | Retained vector element capacity; count, not bytes |
| `snapshots.slot_read_records` | Nonempty individual cached slot records, not controller count times four |
| `snapshots.setup_plan_groups`, `snapshots.setup_journal_groups` | Group elements retained by the active plan and interruption journal |
| `fixed_storage.units` | `"bytes"` |
| `fixed_storage.accounting` | `"sizeof_embedded_objects_not_additional_heap"` |
| `fixed_storage.packet_queue_object_bytes` | Target compiler's `sizeof(ControllerPacketQueue)`, including its fixed packet array, mutex and bookkeeping |
| `fixed_storage.duplicate_window_object_bytes` | `sizeof(ControllerDuplicateWindow)` |
| `fixed_storage.mesh_capture_object_bytes` | `sizeof(MeshCapture)` |
| `fixed_storage.binding_refresh_scratch_bytes` | `sizeof` the two-round binding records and count-response arrays |
| `packet_queue.capacity`, `packet_queue.maximum_packet_bytes`, `packet_queue.queued` | Event packet queue's capacity, payload bound, and locked read-only occupancy sample |

Fixed objects are embedded in the hub; their bytes can already reside in a heap
allocation for that hub. They are not extra free-heap deductions to apply to the
measured values. Their sizes include target-specific alignment and mutex layout;
host `sizeof` is not an ESP32 RAM measurement. Map entries/vector capacity expose
retention and workload but omit allocator nodes, string capacity and overhead.
Only the heap APIs measure available heap. These counts are not additive byte
estimates and do not establish which controller caused a heap change.

## Staged before/after comparison

No physical measurement, upload, inventory change or restart is performed by
this revision. Once a controlled session is started, retain raw status replies
privately at boot/idle, after normal warm-up, during several active controllers,
after a bounded burst and after recovery. Record the existing event/drop counters
alongside heap/stack fields, map/snapshot counts, uptime, reset reason and the
actual number of devices, groups and active controllers. Use an inventory
comparable to the reported 81-device installation; a smaller home inventory does
not answer that scaling question.

Compare baseline and candidate on the **same board, YAML/configuration, library
revision, compiler/ESP-IDF/ESPHome versions, inventory, exposure/mode settings,
warm-up interval, status sampling schedule and input workload**. When the
candidate itself changes the library, record both exact revisions and identify
that deliberate variable. Record actual firmware hashes and the calling task's
name; do not compare stack minima from different tasks as one workload.

An idle/active subtraction is a difference between current-free samples, not a
resettable local minimum. A boot-lifetime minimum remains influenced by every
earlier phase, including previous diagnostic responses. For a before/after
minimum comparison use independently recorded equivalent boot/workload runs;
never reset the production allocator minimum to manufacture a phase result.
Missing telemetry or dropped input makes that part of the measurement incomplete.
Retain production inventory and controller assignments throughout.

Host tests verify unavailable/null output, byte units and distinct metric
selection through ESP-IDF API stubs, stack-metric disabled builds, an absent
PSRAM pool versus an exhausted existing heap, and repeated management responses
with an 81-device synthetic inventory. The latter verifies that status reporting
preserves queued packets, duplicate history, local baselines, binding snapshots,
capture state and inventory while sending no mesh operation. It does not measure
ESP32 heap, stack margins, concurrent hardware traffic or end-to-end delivery.
