"""ESPHome external component for Avi-on BLE mesh lights (C++20).

Auto-scans for Avi-on bridges and connects to the one with the best RSSI.
Reconnects automatically on disconnect.
"""

import base64
import binascii
import gzip
import os

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32_ble
from esphome.components.esp32 import add_idf_sdkconfig_option
from esphome.const import CONF_ID

DEPENDENCIES = ["esp32", "mqtt"]
AUTO_LOAD = ["json", "esp32_ble", "web_server_base"]

CONF_PASSPHRASE = "passphrase"


def validate_passphrase(value):
    """Validate that the passphrase meets minimum requirements.

    Accepts both base64-encoded strings (new format) and any string ≥8 characters.
    For base64 strings, validates encoding and checks decoded length ≥ 16 bytes.
    """
    if not value:
        return value

    # Minimum 8 characters
    if len(value) < 8:
        raise cv.Invalid(f"Passphrase must be at least 8 characters, got {len(value)}")

    # Check if it looks like base64 (length multiple of 4)
    if len(value) & 3 == 0:
        try:
            # Try to decode as base64
            decoded = base64.b64decode(value, validate=True)
            # If it's valid base64, enforce minimum decoded length
            if len(decoded) < 16:
                raise cv.Invalid(
                    f"Base64 passphrase must decode to at least 16 bytes, got {len(decoded)} bytes"
                )
        except (binascii.Error, ValueError):
            # Not valid base64 - accept it anyway (old hex format or other)
            pass

    return value


esp32_ble_ns = cg.esphome_ns.namespace("esp32_ble")
GAPScanEventHandler = esp32_ble_ns.class_("GAPScanEventHandler")

avionmesh_ns = cg.esphome_ns.namespace("avionmesh")
AvionMeshHub = avionmesh_ns.class_(
    "AvionMeshHub",
    cg.Component,
    esp32_ble.GAPEventHandler,
    GAPScanEventHandler,
    esp32_ble.GATTcEventHandler,
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(AvionMeshHub),
        cv.GenerateID(esp32_ble.CONF_BLE_ID): cv.use_id(esp32_ble.ESP32BLE),
        cv.Optional(CONF_PASSPHRASE): validate_passphrase,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    parent = await cg.get_variable(config[esp32_ble.CONF_BLE_ID])
    cg.add_define("USE_ESP32_BLE_CLIENT")
    add_idf_sdkconfig_option("CONFIG_BT_GATTC_ENABLE", True)
    add_idf_sdkconfig_option("CONFIG_BT_GATTS_ENABLE", True)

    # Passphrase is now optional - stored in NVS instead of config
    if CONF_PASSPHRASE in config:
        cg.add(var.set_passphrase(config[CONF_PASSPHRASE]))

    # Download libraries via lib_deps (compiled via fix_cmake.py for ESP-IDF)
    cg.add_platformio_option("lib_deps", [
        "https://github.com/oyvindkinsey/avionmesh-cpp.git"
    ])

    # C++20 for the libraries
    cg.add_build_flag("-std=gnu++20")
    esp32_ble.register_gap_event_handler(parent, var)
    esp32_ble.register_gap_scan_event_handler(parent, var)
    esp32_ble.register_gattc_event_handler(parent, var)

    # Patch src/CMakeLists.txt to add mbedtls dependency for ESP-IDF linking
    script_path = os.path.join(os.path.dirname(__file__), "fix_cmake.py")
    cg.add_platformio_option("extra_scripts", [f"pre:{script_path}"])

    # Gzip web assets → C headers
    comp_dir = os.path.dirname(__file__)

    def _embed_asset(src_name, header_name, symbol):
        src = os.path.join(comp_dir, src_name)
        hdr = os.path.join(comp_dir, header_name)
        with open(src, "rb") as f:
            raw = f.read()
        compressed = gzip.compress(raw, compresslevel=9)
        hex_lines = []
        for i in range(0, len(compressed), 16):
            chunk = compressed[i : i + 16]
            hex_lines.append(", ".join(f"0x{b:02x}" for b in chunk))
        with open(hdr, "w") as f:
            f.write("#pragma once\n")
            f.write("#include <cstdint>\n")
            f.write("#include <cstddef>\n\n")
            f.write(f"// Auto-generated from {src_name} — do not edit\n")
            f.write(f"static const uint8_t {symbol}[] = {{\n")
            f.write(",\n".join(f"    {line}" for line in hex_lines))
            f.write("\n};\n\n")
            f.write(f"static const size_t {symbol}_SIZE = {len(compressed)};\n")

    _embed_asset("web.html", "web_content.h", "AVIONMESH_WEB_HTML")
    _embed_asset("web.css",  "web_style.h",   "AVIONMESH_WEB_STYLE")
    _embed_asset("web.js",   "web_script.h",  "AVIONMESH_WEB_SCRIPT")
