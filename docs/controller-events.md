# Controller protocol and HA discovery foundation

This draft supplies protocol decoders, bounded packet/duplicate queues, assignment snapshots and MQTT discovery builders. The dependent controller integration PR connects these helpers to BLE, claiming and the web UI. This foundation alone does not enable live controller events.

Profiles distinguish Accessory Dimmer (91), Anywhere touch slider (81), battery scene keypad (126) and wired scene keypad (127) from load devices such as Smart Dimmer (97). The recorded retail families are HIWAC, HWAS, HIWSKB and HIWSKA respectively; exact retail suffixes have not been confirmed from device labels. Native commands include target IDs; named keypad buttons require verified distinct slot assignments. Auxiliary On/Off/Raise/Lower operate on the last selected target. No hold/release gesture is inferred.

The decoder's trust precondition is an authenticated, decrypted mesh source resolved to its inventory product. Neither payload bytes nor an advertised identity establish that product. Call `decode_controller_command(authenticated_product, opcode, payload, length, result)` only after that lookup; pass product 0 when the source is unknown or untrusted. This overload rejects unknown sources and load products, including 162. The older overload without a product argument remains compact-only for compatibility and requires the caller to enforce the same trusted-controller precondition. The helper itself performs no cryptographic authentication.

Two ten-byte absolute Write/Dimming payloads are recognized with opcode `0x73`:

| Payload | Proven product/mode | Level |
| --- | --- | --- |
| `00 0a GG GG 00 LL 00 00 00 00` | Native products 91 and 81, assigned target | Byte 5 |
| `00 0a GG GG 01 00 LL 00 00 00` | Native product 91, assigned group target | Byte 6 |

`GG GG` is the big-endian target ID. The second layout requires inventory product 91, marker byte 4 equal to 1, byte 5 equal to 0, and all three trailing bytes equal to 0. Byte 5 has no established meaning beyond that observed zero. Other lengths, markers and padding stay generic `mesh_command` events; there is no inferred 16-bit level or shifted relative command. The compact relative form `11 0a GG GG 00 SS 00 00 00 00` retains the established `0d` raise and `f3` lower steps. Ordered absolute levels feed direction tracking directly; no ramp depends on load echoes.

Model 91 can additionally report local level as `01 0a TT LL`. `decode_local_dimmer_report` has the same authenticated-source/inventory precondition and accepts only product 91. Product-162 loads have also been observed emitting four-byte endpoint echoes `01 0a 00 LL`; matching bytes from a load must never become a local dimmer input. Tagged read replies establish a silent baseline; zero-tag unsolicited reports preserve repeated equal-level On/Off actions. A recently observed zero-tag query from another client conservatively suppresses ambiguous replies. This cannot guarantee delivery through every mesh relay or RF environment, and another native 91 capture emitted no such reports.

Physical provenance was checked against private saved captures on October 30, 2026. Sanitized source labels below replace installation identities; private source IDs, target IDs, keys and raw inventories are not published:

| Source | Authenticated inventory product | Mode and observed evidence |
| --- | --- | --- |
| A | 91 | Assigned native Accessory Dimmer: 51 compact absolute commands to a group. The compact form is therefore not bridge-only. |
| B | 81 | Assigned native Anywhere Dimmer: 20 compact commands to a group. |
| C | 91 | Fresh unassigned Accessory Dimmer: four-byte levels `ff,c5,ff,00`; a separate confirmed repeat test recorded `ff,ff,00,00` at distinct receipt times. |
| Maintainer's Accessory Dimmer | 91 | Additional ten-byte absolute form to group 272, including endpoints and intermediate ramp levels. [Public capture report](https://github.com/oyvindkinsey/esphome-avionmesh/pull/24#issuecomment-6515140432). |
| Maintainer's loads | 162 | Four-byte endpoint echoes; the native 91 in that installation emitted none. [Public source-attribution report](https://github.com/oyvindkinsey/esphome-avionmesh/pull/24#issuecomment-6515240370). |

Fixtures use public packet shapes and synthetic targets, levels, sequences and timestamps; they contain no mesh keys or private installation inventory. Offline regression coverage includes both absolute layouts, endpoints, intermediate ramps, direction changes, malformed variants, product/unknown-source gating, source/sequence de-duplication, freshness, repeated reports, slot ambiguity and MQTT discovery. Queue saturation drops newly arriving packets while preserving the retained FIFO and receipt timestamps; fixtures exercise wraparound and recovery after a sustained flood. A bounded queue does not guarantee lossless input delivery, and freshness checks still apply after dequeue.

These fixtures do not replace physical validation of additional firmware revisions, relay conditions, or sustained multi-controller traffic. No new physical test or firmware flash is part of this decoder revision.
