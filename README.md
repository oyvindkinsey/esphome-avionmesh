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

#### Starting Fresh

1. Flash the firmware to your ESP32
2. Navigate to `http://avionmesh.local/ui` in your browser
3. Use the **Generate** button to create a mesh passphrase and save it somewhere safe
4. Use the web UI to add devices:
   - **Scan Unassociated** - Find unpaired devices in range
   - **Claim Device** - Pair discovered devices with custom names
   - **Manage Groups** - Organize devices into groups

#### Import from Avi-on

If you already have devices set up in the Avi-on app, you can import your existing device names, groups, and passphrase directly from the Avi-on cloud.

> **Security notice:** Review the script before running it — it will authenticate to the Avi-on API with your credentials and POST data to your ESPHome device.

Download and inspect the script first (recommended), then run it:

```bash
curl -O https://raw.githubusercontent.com/oyvindkinsey/esphome-avionmesh/master/tools/avion_import.py
# Review avion_import.py before continuing
python3 avion_import.py --email YOUR_EMAIL --password YOUR_PASSWORD --device avionmesh.local
```

The script imports your passphrase, device names, and group assignments. Once complete, your devices will appear in Home Assistant via MQTT without any re-pairing.

##### Validating group membership (optional)

The Avi-on cloud stores group assignments server-side, but the actual membership is programmed into each device's firmware. These can drift out of sync. To validate and repair group memberships over BLE before importing, use the [`avionmesh`](https://github.com/oyvindkinsey/avionmesh) library:

1. Install the library (requires Python 3.11+, a BLE adapter, and proximity to your mesh):

   ```bash
   pip install "avionmesh[cloud]"
   ```

2. Run the TUI and import from cloud (option **2**):

   ```bash
   mesh-tui
   ```

   This creates `mesh_db.json` in the current directory with your passphrase, devices, and groups.

3. Still in `mesh-tui`, select option **8 — Repair group memberships**. This connects to the mesh over BLE, reads the actual group assignments programmed into each device, and updates `mesh_db.json` to reflect the real state.

4. Push the validated database to your ESPHome device:

   ```bash
   python3 avion_import.py --from-file mesh_db.json --device avionmesh.local
   ```

## Usage

### Web Management Interface

Access the web UI at `http://avionmesh.local/ui`:

- **Status** - View connection state, bridge info, and device list
- **Discover Mesh** - Scan for all devices on the mesh network
- **Scan Unassociated** - Find new, unpaired devices ready for claiming
- **Claim Device** - Add discovered devices with custom names
- **Manage Groups** - Create, delete, and modify device groups
- **Control Panel** - Test on/off, brightness, and color temperature

### MQTT Integration

Devices are **not** exported to MQTT by default. Enable MQTT for individual devices and groups via the checkboxes in the web UI — changes take effect immediately without a restart.

Each exported device appears as a light in Home Assistant with:

- **Power** (`state`) - On/off control
- **Brightness** (`brightness`) - 0-255
- **Color Temperature** (`color_temp`) - Mireds (153-370)

#### Color Temperature (Mireds to Kelvin)

The component handles the mireds/kelvin conversion automatically:

- Discovery advertises mireds range (`min_mireds`, `max_mireds`)
- Commands accept mireds (Home Assistant standard)
- State publishes mireds (converted from the mesh's internal kelvin values)

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
          topic: "avionmesh/light/0/set"
          payload: '{"color_temp": 370}'  # Warmest (2700K)
```

## Configuration Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `passphrase` | string | *auto-generated* | Mesh network encryption key. Must be base64-encoded, decoding to ≥16 bytes. Use the web UI "Generate" button to create a valid key, or use your existing Avi-on passphrase from the mesh database. |

## Supported Devices

This component works with Avi-on Bluetooth mesh lighting products and compatible devices.

## Dependencies

- **Avi-on mesh protocol** — [avionmesh-cpp](https://github.com/oyvindkinsey/avionmesh-cpp) (C++, used at build time) / [avionmesh](https://github.com/oyvindkinsey/avionmesh) (Python, provides `mesh-tui`)
- **CSRMesh BLE transport** — [recsrmesh-cpp](https://github.com/oyvindkinsey/recsrmesh-cpp) (C++) / [recsrmesh](https://github.com/oyvindkinsey/recsrmesh) (Python)

## License

LGPL-3.0 License - See LICENSE file for details

## Contributing

Contributions welcome! Please open issues or submit pull requests.