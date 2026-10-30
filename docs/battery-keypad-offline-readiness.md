# Battery keypad 126: offline readiness and staged acceptance

This preparation contacts no device and starts no listener, button prompt,
flash, upload or restart. All physical acceptance below remains pending until
the owner starts the consolidated basement session. Host fixtures establish
software behavior, not automatic programming or physical input delivery.

## Current capabilities

| Capability | Model 126 behavior |
| --- | --- |
| Passive command decoding | Supported for observed wire forms; native control remains in the keypad |
| Assignment reads | Explicit read request waits for wake, then COUNT plus all four slots twice |
| Named HA buttons | Available only for a valid model-matching map with distinct supported targets |
| Shared-target presets/scenes | Generic commands remain usable; brightness percentages do not prove physical button identity |
| Automatic or manual assignment writes | Unsupported; no experimental write path is enabled |
| Automatic unused-slot HA targets | Unsupported; no groups are reserved for model 126 |
| Sleep and first input delivery | Hardware acceptance pending; a read timeout does not mean empty or unsupported assignments |

`controller_buttons` JSON and HA button `/mapping` attributes report
`assignment_read_supported`, `assignment_write_supported`, `wake_required` and
`programming_status`. For 126 these are `true`, `false`, `true` and
`programming_unverified`. A slot's `mapping_status: ready` describes its verified
native mapping; it does not claim that programming was verified. Setup remains
`programming_unverified` after a successful read or restart, even when all four
native slots can produce named events. A stored historical setup-pending flag
cannot authorize battery writes.

A read invalidates the old map before waiting. Its wake must come from the
receive path's authenticated inventoried model-126 source, after this request,
at opcode `0x73`, with the observed seven-byte `0b1c` header shape and at most
one second of queue age. The reference fixture is `0b1c010b056401`; meanings and
constancy of its trailing fields are unknown. Varying them is allowed for this
read-only wake classifier. Neither an ordinary brightness packet, another
source, an old queued packet nor a wake after the three-minute request deadline
opens a receive window.

After wake the reader settles for 150 ms. Each battery table request has a
750 ms deadline (including queued replies), with at most two timeout retries.
It never retries a write. Two complete rounds must agree, record indexes must
match all four slots, and COUNT must match occupied records before saving.
A disconnect, mesh change, controller removal or product change cancels this
reader and its outstanding request. Timeout/failure stops the attempt; a new
explicit read starts a new bounded wake wait. Restart reloads saved records;
it does not resume an interrupted battery read or initiate assignment writes.
A new refresh still requires a new wake.

## Owner-started read/event acceptance (pending)

Record exact firmware/configuration, inventory product and authenticated source,
raw packet timestamps, table requests/replies, MQTT/HA events and observed native
loads privately. Keep the current native Button 1 baseline. Use the existing
session's isolation/baseline/rollback procedure; this checklist executes nothing.

1. Capture a fresh full baseline: COUNT and slots 1–4 twice, matching records and
   occupied count. Preserve every raw record, including unknown/scene records.
   The last verified development baseline was one native Button 1 and three
   unused slots; re-establish it instead of assuming it remains current.
2. Start the explicit read with the vendor app closed. Observe waiting, the
   authenticated wake, settling, ten successful requests and saved status. Do
   not label a missing wake an unsupported table or an empty assignment.
3. Let a separate read expire without wake. Verify no COUNT/READ was sent, no
   targets/groups were created and existing native Button 1 behavior remains.
   Record missed first input separately from table-read failure.
4. For slots 1, 2, 3 and 4, record what each physical input actually sends. An
   unused slot may send nothing; that is not a successful named HA input. If
   targets overlap, require ambiguous status and no individual named-button
   claim, even when their levels are 100/75/50/25 percent. Record target, command
   and brightness without using brightness alone to assign a button identity.
5. After each populated native target, record auxiliary On/Off/Raise/Lower and
   its last-target context. Do not infer hold/release or independent physical
   slots from auxiliary commands. If an HA-only target already exists from a
   separately verified configuration, record load isolation and auxiliary
   behavior there as a distinct case; this suite does not program that target.
6. After a recorded sleep interval, record the first native press, first event
   and wake timing, then repeat presses/auxiliary controls. Missing receptions
   remain missing; do not silently discard the first attempt. Repeat the explicit
   assignment read and compare all four records to baseline.
7. During a read, use the session's planned controlled disconnect/restart stage.
   Confirm stopped/uncertain status, no resumed battery writes, restored saved
   mapping/exposure and a new wake requirement for the next explicit refresh.
   Record first input after recovery and absence of stale replay separately.
8. Compare native Button 1, all four records, groups and unrelated-controller
   state against baseline. No battery assignment or group changes are expected.

## Optional programming research (separate; unsupported)

Observed native-app primary/secondary inserts and acknowledgements establish
candidate wire traffic only. They do not prove automatic battery write,
readback, physical mixed-mode operation or restoration. No compile gate or
user-facing write action is provided by this offline revision.

Before adding even a default-off experimental path, retain a complete native
backup (COUNT, every slot, available secondary/scene records and firmware/model),
a complete authenticated app request/reply exchange with fresh full readback,
and a reviewed restoration procedure. Establish unknown status/secondary-field
semantics, correlation and wake-window boundaries. Offline tests must exercise
journal-before-send, no occupied-slot replacement (especially native Button 1),
owned empty-target collision protection, single write attempt, ACK-without-save,
late/duplicate/foreign replies, disconnect/restart reconciliation and full-table
readback preserving every untouched record. An explicit user-started test and
compile-time default-off gate would be required. Successful unused-slot write,
physical HA-only input with no native load change, sleep/restart behavior, and
restoration readback/physical operation must then be established on hardware.
Until those independent checks pass, model-127 programming success cannot be
used as model-126 acceptance evidence.
