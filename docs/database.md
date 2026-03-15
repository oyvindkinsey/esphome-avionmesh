# Database

## Data Model

```
DeviceEntry
├── avion_id:       uint16   (range 32896–65407)
├── product_type:   uint8
├── name:           string
├── groups:         uint16[] (group IDs this device belongs to)
├── mqtt_exposed:   bool
└── min_brightness: uint8    (0 = disabled; non-zero clamps brightness to this floor)

GroupEntry
├── group_id:      uint16   (range 256–24575)
├── name:          string
├── member_ids:    uint16[] (avion_ids)
└── mqtt_exposed:  bool
```

The broadcast entity (ID 0 / "All Lights") is not stored in the DB — its `mqtt_exposed` flag is a separate NVS key.

## NVS Persistence

Namespace: `avionmesh`

| Key | NVS type | Content |
|-----|----------|---------|
| `devices` | blob | binary device list |
| `groups` | blob | binary group list |
| `passphrase` | string | base64 passphrase |
| `mesh_mqtt` | uint8 | 1 = broadcast entity is MQTT-exposed |

Every mutation (add/remove device or group, group membership change, passphrase set/generate) triggers an immediate `save()`.

### Binary Format (little-endian)

Format version byte precedes the device count. Current version: `1`.

```
Devices: [version u8] [count u16] ( [avion_id u16] [product_type u8] [flags u8] [min_brightness u8]
                                     [name_len u8] [name...] [group_count u16] [group_id u16 * N] ) * count

Groups:  [count u16] ( [group_id u16] [flags u8]
                        [name_len u8] [name...] [member_count u16] [member_id u16 * N] ) * count

flags: bit 0 = mqtt_exposed
On load, version 0 (legacy) blobs are accepted with min_brightness defaulting to 0.
```

## Passphrase

- Stored as a base64 string; must decode to ≥ 16 bytes
- If provided in YAML (`passphrase:`) and NVS is empty, copied to NVS on first boot
- Generated via 16 random bytes → mbedTLS base64 encode
- All mesh crypto operations are disabled until a passphrase is set
