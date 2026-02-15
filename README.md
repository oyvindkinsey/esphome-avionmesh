# ESPHome Avi-on Mesh Component

An ESPHome custom component that provides **complete local control** of Avi-on Bluetooth mesh lighting products through **Home Assistant MQTT**, eliminating all dependency on Avi-on's proprietary cloud infrastructure.

## Features

- **100% Local Control** - No cloud, app accounts, or internet required
- **Home Assistant Integration** - Auto-discovery via MQTT for lights, groups, and broadcast control
- **Individual Device Control** - On/off, brightness, and color temperature for each light
- **Group Control** - Manage groups of devices with a single command
- **Broadcast Control** - Control all mesh devices simultaneously
- **Color Temperature Without Disturbance** - Adjust warmth of devices, groups, or entire mesh **without** changing on/off/brightness state (perfect for circadian lighting automations)
- **Web Management UI** - Built-in interface for device discovery, pairing, group management, and real-time status
- **Persistent Device Database** - Device and group configuration stored in NVS

## Why Use This?

Avi-on's official ecosystem requires their mobile app and cloud service, which creates vendor lock-in and puts your smart home at the mercy of a third-party service. This component:

1. **Eliminates vendor lock-in** - Own your devices fully, independent of Avi-on's business decisions
2. **No cloud dependency** - Everything works locally over your LAN; no risk of service shutdowns, API changes, or sunsets
3. **Simplifies management** - Web UI is faster and more capable than the mobile app
4. **Enables automation** - Full MQTT integration means Home Assistant automations just work
5. **Improves color temperature control** - The official app forces devices ON when changing warmth; this component adjusts color temperature silently in the background
6. **Reduces latency** - Direct BLE-to-gateway communication is faster than cloud round-trips

## Hardware Requirements

- ESP32 board (POE-supported boards like `esp32-poe` recommended for always-on gateways)
- Avi-on BLE mesh devices

## Installation

### Minimal Configuration

A minimal configuration for the Avi-on gateway:

```yaml
esphome:
  name: avionmesh
  friendly_name: "Avi-on Mesh"

esp32:
  board: esp32-poe
  framework:
    type: esp-idf

# Enable logging
logger:

# Enable Home Assistant API
api:
  encryption:
    key: !secret api_encryption_key

# Enable Over-The-Air updates
ota:
  password: !secret ota_password

# Enable WiFi or Ethernet (example shows Ethernet)
ethernet:
  type: LAN8720
  mdc_pin: GPIO23
  mdio_pin: GPIO18
  clk_mode: GPIO17_OUT
  phy_addr: 0
  power_pin: GPIO12
  use_address: "avionmesh.local"

# MQTT broker (required for Home Assistant discovery)
mqtt:
  broker: !secret mqtt_broker
  port: 1883

# Enable the web server for the management UI
web_server:
  version: 3

# Load required external components
external_components:
  - source: github://oyvindkinsey/esphome-avionmesh
    components: [avionmesh]

# Avi-on mesh configuration
avionmesh:
  # Optional: Set a mesh passphrase (default: auto-generated)
  # Must be base64-encoded, decodes to ≥16 bytes
  # passphrase: "dfj4nNQJwZ3jw5ZlahvSWk5GeDLU71NyQrHY5vCDr+VTDNBnsTIuIssNWvTxuWQ+pTtEAs43NsBc2ovV0rLJ5A=="
```

### Initial Setup

1. Flash the firmware to your ESP32
2. Navigate to `http://avionmesh.local/avionmesh` in your browser
3. The web UI will guide you through:
   - Scanning for Avi-on bridge devices
   - Discovering unpaired mesh devices
   - Claiming (pairing) devices
   - Creating groups
   - Testing controls

## Usage

### Web Management Interface

Access the web UI at `http://avionmesh.local/avionmesh`:

- **Status** - View connection state, bridge info, and device list
- **Discover Mesh** - Scan for all devices on the mesh network
- **Scan Unassociated** - Find new, unpaired devices ready for claiming
- **Claim Device** - Add discovered devices with custom names
- **Manage Groups** - Create, delete, and modify device groups
- **Control Panel** - Test on/off, brightness, and color temperature

### MQTT Integration

Devices are automatically exposed to Home Assistant via MQTT discovery. Each device appears as a light with:

- **Power** (`state`) - On/off control
- **Brightness** (`brightness`) - 0-255
- **Color Temperature** (`color_temp`) - Mireds (153-370)
```

#### Color Temperature (Mireds to Kelvin)

The component handles the mireds/kelvin conversion automatically:

- Discovery advertises mireds range (`min_mireds`, `max_mireds`)
- Commands accept mireds (Home Assistant standard)
- State publishes kelvin values internally to the mesh

Example automation (change warmth without turning on):

```yaml
automation:
  - alias: "Evening Warm Lighting"
    trigger:
      - platform: time
        at: "19:00:00"
    action:
      - service: mqtt.publish
        data:
          topic: "avionmesh/broadcast/set"
          payload: '{"color_temp": 370}'  # Warmest (2700K)
```

## Configuration Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `passphrase` | string | *auto-generated* | Mesh network encryption key. Must be base64-encoded, decoding to ≥16 bytes. Use the web UI "Generate" button to create a valid key, or use your existing Avi-on passphrase from the mesh database. |

## Supported Devices

This component works with Avi-on Bluetooth mesh lighting products and compatible devices.

## How It Works

1. **BLE Gateway** - The ESP32 scans for Avi-on bridge devices and connects to the strongest signal
2. **Mesh Protocol** - Communicates directly with Avi-on mesh devices using the proprietary protocol
3. **MQTT Bridge** - Translates MQTT commands to mesh packets and vice versa
4. **Persistent Storage** - Device database saved to NVS for quick recovery after reboot

## Troubleshooting

### Devices not discovered

- Ensure the ESP32 is within BLE range of your Avi-on devices
- Check that devices are in pairing mode (usually: power cycle 5x rapidly)
- Verify the mesh passphrase matches if migrating from another controller

### Migrating from Avi-on

To keep your existing paired devices, use the same passphrase:

1. Extract your existing passphrase from the Avi-on mesh database (stored as base64)
2. Set it in your YAML: `passphrase: !secret avionmesh_passphrase`
3. Or set it via the web UI after flashing

The mesh passphrase is the encryption key that allows communication with your paired devices.

### Connection drops

- The component auto-reconnects to bridges with the best RSSI
- Check logs for BLE connection errors
- Consider adding a second gateway for larger mesh networks

### MQTT not working

- Confirm MQTT broker is running and accessible
- Verify `mqtt:` section in your YAML
- Check Home Assistant's MQTT integration is enabled

## License

LGPL-3.0 License - See LICENSE file for details

## Contributing

Contributions welcome! Please open issues or submit pull requests.

## Acknowledgments

- Based on reverse engineering of Avi-on's Bluetooth mesh protocol
- Built with ESPHome's external component system
