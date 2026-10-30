# Dimmer event readiness and assignment boundaries

The controller profiles cover authenticated inventory products 91 (Accessory
Dimmer) and 81 (Anywhere/Anyplace touch slider). An event capability identifies
an observed packet format. It does not establish the current native assignment,
physical isolation, wake behavior, or a press/release gesture.

| Product/path | Named events | Baseline | Native assignment programming |
| --- | --- | --- | --- |
| 91 native commands | Brightness, On, Off, inferred Raise/Lower | First accepted command emits Brightness; direction needs a recent same-target level | Unsupported; format unknown |
| 91 local reports | Brightness, On, Off, inferred Raise/Lower | First report is silent; tagged queries silently seed/refresh | Unsupported; selecting this path does not clear assignments |
| 81 native slider | Brightness, On, Off, inferred Raise/Lower | First accepted command emits Brightness; direction needs a recent same-target level | Unsupported; format unknown |
| 81 unassigned reports | No verified decoder | Unknown | Unsupported; spare capture required |

`controller_profile.h` projects these observed capabilities. `ha_input_status`
describes exposure, decoder selection and (for local reports) baseline readiness.
`native_commands` means the native decoder is selected; it is not a live-device
or assignment verification result. API dimmer metadata explicitly reports
`ha_native_assignment_status: unknown` and
`ha_native_assignment_programming: false`. A successful claim response projects
the persisted settings, not assumed completed commissioning.

Fresh claims of 91 select local reports. Imported/native devices keep their
existing mode. Neither exposure nor mode selection programs native assignments.
The Office product-81 slider's existing managed empty target 757 remains untouched.
No four-slot keypad format is reused for either dimmer.

Local-report readiness is revoked when the mesh/MQTT path is lost or the mode
changes. On loss, the GUI receives `waiting_for_baseline` once; with MQTT still
connected, per-input availability is retained offline immediately. MQTT reconnect
discovery publishes this offline baseline state before shared controller
availability can return online. BLE recovery alone cannot restore readiness.
Repeated disconnected loops do not repeat the notification/publication. Until a tagged read response arrives, the first physical action may be
consumed as a silent baseline. It cannot safely be identified as an action from
its bytes alone. Once ready, ordered zero-tag reports can repeat at unchanged
endpoints; tagged queries, duplicates and reordered packets do not emit inputs.
External zero-tag queries have a conservative two-second exclusion window.
Direction describes a level change and does not identify a physical gesture.

Brightness discovery uses the same stable entity/trigger IDs in both 91 modes.
Only local report mode adds the per-input baseline availability requirement.
Legacy command events and native lighting assignments retain their behavior.

## Offline completion and deferred evidence

Software can be completed and tested without USB or controller access: model
capabilities, decoding captured frames, conservative baseline handling, discovery,
status/API projection and assignment-preserving UI. Native host fixtures cover
these paths. A later firmware build can also run offline, without upload.

The following remain evidence collection in one future user-authorized basement
session. This document schedules no work and starts no listener or broker job.

| Case | Required evidence | Access needed |
| --- | --- | --- |
| 91 fresh local baseline and first action after reconnect | Capture baseline response before the action; compare named/legacy events and HA timestamps. Also characterize action-before-baseline suppression. | Existing bridge LAN capture plus physical access; USB is not intrinsically needed |
| 91 native absolute layout | Confirm compact/shifted source-authenticated frames produce named Brightness and correct endpoints, repeated actions and direction on the installed revision; verify original target still operates | Physical access, bridge capture, later approved firmware installation if required |
| Existing native 81 / Office 757 | Confirm slider event level and target are unchanged, protected target remains empty, Office-only automation follows it, other loads stay unchanged | Physical access and bridge/HA observations; no reassignment |
| Spare unassigned 81 | Record first claim/reconnect, wake-first action, slider endpoints and intermediate levels without assuming a 91 report format | Spare plus phone/app workflow capture if local claim yields no observed format |
| 91 native assignment protocol | Successful reversible vendor-app change with request/reply, fresh readback and restoration, including raw unknown bytes | Spare/test controller and phone-side or equivalent capture that actually exposes app writes |
| 81 native assignment protocol | Independently establish successful save and restore, wake timing, indexing and acknowledgement/readback semantics | Spare plus phone-side or equivalent capture; 91/keypad formats are not substitutes |

USB is only needed if the chosen later installation/recovery workflow requires
it. It cannot resolve an unknown vendor assignment format by itself. Prior bridge
relay recordings did not expose the vendor app's assignment writes; another
identical passive relay recording is not sufficient evidence for implementing
native programming. Silence on a read remains unknown, never an empty assignment.
