# Controller integration and wired keypad setup (draft)

Requires the controller protocol/discovery foundation. Controllers appear as HA event inputs instead of lights. New controller claims enable HA events; imported controllers retain native assignments. Existing exposure flags persist before the UI acknowledges a change, with rollback on save failure.

For wired scene keypad model 127, claim or a disabled-to-enabled HA transition queues setup. Setup reads COUNT and all four slots twice, requires matching snapshots, preserves every occupied/unknown slot, and gives each verified unused slot an owned empty group target. The engine journals before writing, rechecks the slot, makes one programming attempt, and verifies the full table twice. Uncertain outcomes require readback reconciliation. Re-running setup reuses owned targets. Reserved targets cannot acquire light members through management operations.

Buttons 1-4 receive named HA events/device triggers only when their target is distinct and the map is verified. On/Off/Raise/Lower refer to the currently selected native target. Native assignments retain direct control; empty targets can be automated in HA to mix both modes.

Accessory Dimmer model 91 claimed locally starts in HA local-level mode and uses silent tagged reads to establish a baseline. Existing native dimmers are not converted automatically. An explicit input-mode toggle selects local reports without writing physical assignments. Anywhere touch-slider model 81 reports absolute brightness and inferred direction while native group commands are visible. Neither model receives speculative keypad-table writes.

Battery scene keypad model 126 retains passive event decoding and wake-aware assignment reads. Programming writes for that model are disabled in this public draft pending further hardware verification. Preset scenes sharing one target cannot reliably identify four physical buttons.

Hardware evidence from development: two wired keypads, including a locally claimed spare, reported named buttons and auxiliary controls; existing direct assignments were preserved. Accessory dimmer repeated On/On/Off/Off produced two On, two Off and four brightness events. Native dimmer and slider inputs were observed. RF coverage affects reception: missed first presses occurred with the bridge on another floor. This is not a universal delivery guarantee.

Management follows the existing LAN trust model. Diagnostic capture is bounded; controller table commands are typed/guarded rather than an arbitrary mesh-write endpoint. No deployment configuration or installation capture is included.
