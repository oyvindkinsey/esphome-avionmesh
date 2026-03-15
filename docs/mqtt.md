# MQTT

## Topic Prefix

`<node_name>` — ESPHome device name (e.g. `avionmesh`). Set from `mqtt.get_topic_prefix()` at startup.

## Per-Device / Per-Group Topics

| Topic | Dir | Payload | Retained |
|-------|-----|---------|----------|
| `<prefix>/light/<id>/state` | pub | `ON` / `OFF` | yes |
| `<prefix>/light/<id>/set` | sub | `ON` / `OFF` | — |
| `<prefix>/light/<id>/brightness/state` | pub | `0`–`255` | yes |
| `<prefix>/light/<id>/brightness/set` | sub | `0`–`255` | — |
| `<prefix>/light/<id>/color_temp/state` | pub | mireds | yes |
| `<prefix>/light/<id>/color_temp/set` | sub | mireds | — |

`<id>` = Avi-on device or group ID (uint16, decimal). Broadcast entity uses ID 0.

## Management Channel

| Topic | Dir |
|-------|-----|
| `<prefix>/avionmesh/command` | sub |
| `<prefix>/avionmesh/response` | pub |

Management commands are JSON. See [API Reference](api.md) for payload schemas.

## HA Auto-Discovery

Discovery topic: `homeassistant/light/<node_name>_<id>/config` (retained)

Remove: publish empty retained payload to the discovery topic.

Re-published automatically when HA comes online (`homeassistant/status` → `"online"`).

| Discovery field | Value |
|-----------------|-------|
| `unique_id` | `<node_name>_<id>` |
| `supported_color_modes` | `["color_temp"]` or `["brightness"]` |
| `min_mireds` / `max_mireds` | `200` / `370` |
| `brightness_scale` | `255` |
| `via_device` | `<node_name>` |

## Opt-in Exposure

Devices and groups are not exposed to MQTT by default. When enabled:

1. Discovery payload published (retained)
2. Current state published immediately to state topics — do not wait for next refresh
3. Command topics subscribed

When disabled: empty retained payload published to the discovery topic.

## Group State Inference

CSRMesh provides no notification that a group command was received — only the resulting per-device state updates are visible. When an external controller (wall switch, Avi-on app) commands a group, the component infers the group state from those individual device updates.

### Algorithm

**Exclusive-witness rule** — when device D reports brightness B:

1. `candidate_groups` = all groups D belongs to.
2. For each candidate group G, find an *exclusive witness*: a member of G that is **not** in any other candidate group **and** has reported brightness B. If found, G was triggered; latch G's state to B.

A device belonging to only one group is its own exclusive witness — it latches that group immediately on first report, without waiting for other members. A device shared across multiple groups is ambiguous until a member exclusive to one of those groups reports.

**Subset-propagation rule** — when group G is latched, also latch any group H where `H.members ⊆ G.members`. This handles nested configurations:

| Scenario | Result |
|----------|--------|
| Wall switch commands inner group G1 | G1 latched; outer group G2 ⊇ G1 is **not** latched (no evidence G2 was involved) |
| Wall switch commands outer group G2 | G2 latched; G1 ⊆ G2 is also latched (all G1 devices received the G2 command) |
| G3 ⊇ G2 ⊇ G1, exclusive G3 member reports | All three latched via fixed-point propagation |

### Limitations

- A group with no exclusive members (all members are also in another group) can never be self-latched; it is only latched via propagation from a triggered superset.
- If a device misses a BLE packet, its state remains stale until the next periodic refresh or the next command.

## Color Temperature

The mesh operates in kelvin (2700–6500 K). The component converts at the boundary:

- Inbound (`color_temp/set`): mireds → kelvin: `K = 1,000,000 / mireds`
- Outbound (state publish): kelvin → mireds: `mireds = 1,000,000 / K`

**Silent color temp:** sending only to `color_temp/set` adjusts warmth without changing on/off state or brightness.
