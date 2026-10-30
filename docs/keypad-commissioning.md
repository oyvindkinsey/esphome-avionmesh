# Controller integration and wired keypad setup (draft)

Requires the controller protocol/discovery foundation. Controllers appear as HA event inputs instead of lights. New controller claims enable HA events; imported controllers retain native assignments. Existing exposure flags persist before the UI acknowledges a change, with rollback on save failure.

For wired scene keypad model 127, claim or a disabled-to-enabled HA transition queues setup. Setup reads COUNT and all four slots twice, requires matching snapshots, preserves every occupied/unknown slot, and gives each verified unused slot an owned empty group target. The engine journals before writing, rechecks the slot, makes one programming attempt, and verifies the full table twice. Uncertain outcomes require readback reconciliation. Re-running setup reuses owned targets. Reserved targets cannot acquire light members through management operations.

Buttons 1-4 receive named HA events/device triggers only when their target is distinct and the map is verified. On/Off/Raise/Lower refer to the currently selected native target. Native assignments retain direct control; empty targets can be automated in HA to mix both modes.

## Mixed native and Home Assistant buttons

A wired keypad can use both paths at once. For example, Button 1 can keep its existing Avi-on group assignment and control those lights directly, while Buttons 2-4 can use empty event targets and trigger Home Assistant automations. Enabling HA command events also reports commands from natively assigned buttons, so an HA automation on Button 1 would run **in addition to** its direct lighting action.

In the mesh web UI (`/ui`), claim the wired keypad or enable **HA command events** on an imported one. The bridge automatically sets up verified unused slots; **Read button assignments** refreshes the four-slot map, and **Set up unused HA buttons** retries that setup. Each ready slot is shown with its native group or HA automation target. In Home Assistant, use the keypad's **Button 1** through **Button 4** event entities or device triggers to create automations for the HA-targeted buttons. On/Off/Raise/Lower have separate event entities and device triggers; they act in the context of the last selected target rather than as independent button assignments.

The current mesh UI does **not** provide a per-button selector for a native group versus an HA target, change an occupied native assignment, or turn an occupied slot into an HA-only slot. It only provisions unused slots and displays the readback. Native group assignments and clearing existing assignments still need the HALO/Avi-on app. This is a remaining gap for app-free commissioning, even though mixed native/HA operation works once the assignments exist.

### How an unused button becomes an HA input

1. The bridge reads the keypad's assignment count and four slot records twice. It proceeds only when both snapshots agree and the count matches the records. Occupied and unrecognized slots are left untouched.
2. For each verified unused slot, it reserves a unique **empty Avi-on group** in its local inventory, named `HA event target: <keypad ID>:<button number>`. The group has no lights, is not exposed as an MQTT light, and gets an unused group ID from the supported range (256-24575). It is a target address for the keypad, not a Home Assistant light or helper.
3. Before any write, the bridge saves the original and proposed four-slot tables in a restart journal and persists the reserved group. It reads the selected slot again and requires the original unused record (`01 30 00 <slot> FF FF 00 00`) to still be present.
4. It sends the observed Avi-on assignment command once, inserting the reserved group ID into that slot with level `FF`; after the primary reply it sends the observed secondary insert for `<slot> | 0x20`. It does not add any lights to the group. The write is restricted to verified wired keypads (model 127), a fresh slot read, and the exact owned empty target. An acknowledgement alone is not treated as success.
5. It reads the entire table twice again. Only the expected new record (`01 30 00 <slot> <group ID, two bytes> 08 FF`) with all other slots unchanged marks the write verified and enables that button's named MQTT/HA trigger. A timeout or ambiguous result stops setup for reconciliation; it does not blindly retry the write. The journal lets a later run compare what the keypad actually retained after a restart.

When that button is pressed, its command addresses the empty group, so no Avi-on light changes. The bridge recognizes the unique target and publishes the corresponding `Button N` activation through MQTT discovery for an automation in Home Assistant. A native button still addresses its populated Avi-on group directly. The empty group must remain empty; the bridge blocks adding lights to owned HA event targets through its management API.

Accessory Dimmer model 91 claimed locally starts in HA local-level mode and uses silent tagged reads to establish a baseline. Existing native dimmers are not converted automatically. An explicit input-mode toggle selects local reports without writing physical assignments. Anywhere touch-slider model 81 reports absolute brightness and inferred direction while native group commands are visible. Neither model receives speculative keypad-table writes.

Battery scene keypad model 126 retains passive event decoding and wake-aware assignment reads. Programming writes for that model are disabled in this public draft pending further hardware verification. Preset scenes sharing one target cannot reliably identify four physical buttons.

Hardware evidence from development: two wired keypads, including a locally claimed spare, reported named buttons and auxiliary controls; existing direct assignments were preserved. Accessory dimmer repeated On/On/Off/Off produced two On, two Off and four brightness events. Native dimmer and slider inputs were observed. RF coverage affects reception: missed first presses occurred with the bridge on another floor. This is not a universal delivery guarantee.

Management follows the existing LAN trust model. Diagnostic capture is bounded; controller table commands are typed/guarded rather than an arbitrary mesh-write endpoint. No deployment configuration or installation capture is included.
