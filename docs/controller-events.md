# Controller protocol and HA discovery foundation

This draft supplies protocol decoders, bounded packet/duplicate queues, assignment snapshots and MQTT discovery builders. The dependent controller integration PR connects these helpers to BLE, claiming and the web UI. This foundation alone does not enable live controller events.

Profiles distinguish Accessory Dimmer (91), Anywhere touch slider (81), battery scene keypad (126) and wired scene keypad (127) from load devices such as Smart Dimmer (97). Native commands include target IDs; named keypad buttons require verified distinct slot assignments. Auxiliary On/Off/Raise/Lower operate on the last selected target. No hold/release gesture is inferred.

Model 91 can additionally report local level as `01 0a TT LL`. Tagged read replies establish a silent baseline; zero-tag unsolicited reports preserve repeated equal-level On/Off actions. A recently observed zero-tag query from another client conservatively suppresses ambiguous replies. This cannot guarantee delivery through every mesh relay or RF environment.

Synthetic fixtures cover strict packet shapes, source/sequence de-duplication, freshness, relative versus absolute levels, repeated reports, slot ambiguity and MQTT discovery. Fixtures contain no mesh keys or installation inventory.
