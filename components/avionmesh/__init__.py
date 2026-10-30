"""ESPHome external component for Avi-on BLE mesh lights (C++20).

Auto-scans for Avi-on bridges and connects to the one with the best RSSI.
Reconnects automatically on disconnect.
"""

import base64
import binascii
import gzip
import os
import json
from urllib.parse import urlsplit
from esphome.core import CORE

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32, esp32_ble
from esphome.components.esp32 import add_idf_sdkconfig_option
from esphome.const import CONF_ID

DEPENDENCIES = ["esp32", "mqtt"]
AUTO_LOAD = ["json", "esp32_ble", "web_server_base"]

CONF_PASSPHRASE = "passphrase"
CONF_SINGLE_DEVICE = "single_device"
CONF_CLOUD_SCALE = "experimental_cloud_scale"


def validate_scale_url(value):
    parsed = urlsplit(cv.string_strict(value))
    try:
        port = parsed.port
    except ValueError as error:
        raise cv.Invalid("invalid test HTTPS port") from error
    if (parsed.scheme != "https" or not parsed.hostname or parsed.username or parsed.password
            or parsed.path not in ("", "/") or parsed.query or parsed.fragment
            or any(c.isspace() for c in value) or (port is not None and port == 0)):
        raise cv.Invalid("experimental cloud scale requires an HTTPS origin without credentials/path/query")
    return value.rstrip("/") + "/"


CLOUD_SCALE_SCHEMA = cv.Schema({
    cv.Required("url"): validate_scale_url,
    cv.Required("ca_certificate"): cv.file_,
    cv.Optional("persist", default=False): cv.boolean,
})


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


avionmesh_ns = cg.esphome_ns.namespace("avionmesh")
AvionMeshHub = avionmesh_ns.class_(
    "AvionMeshHub",
    cg.Component,
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(AvionMeshHub),
        cv.GenerateID(esp32_ble.CONF_BLE_ID): cv.use_id(esp32_ble.ESP32BLE),
        cv.Optional(CONF_PASSPHRASE): validate_passphrase,
        cv.Optional(CONF_SINGLE_DEVICE, default=False): cv.boolean,
        cv.Optional(CONF_CLOUD_SCALE): CLOUD_SCALE_SCHEMA,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    if CONF_CLOUD_SCALE in config:
        scale = config[CONF_CLOUD_SCALE]
        with open(CORE.relative_config_path(scale["ca_certificate"]), encoding="ascii") as handle:
            certificate = handle.read()
        if "-----BEGIN CERTIFICATE-----" not in certificate or "PRIVATE KEY" in certificate:
            raise cv.Invalid("ca_certificate must contain only the public test certificate")
        cg.add_define("AVIONMESH_CLOUD_SCALE_TEST")
        if scale["persist"]:
            cg.add_define("AVIONMESH_CLOUD_SCALE_PERSIST")
        cg.add_define("AVIONMESH_CLOUD_SCALE_URL", cg.RawExpression(json.dumps(scale["url"])))
        cg.add_define("AVIONMESH_CLOUD_SCALE_CA", cg.RawExpression(json.dumps(certificate)))
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    parent = await cg.get_variable(config[esp32_ble.CONF_BLE_ID])
    cg.add_define("USE_ESP32_BLE_CLIENT")
    add_idf_sdkconfig_option("CONFIG_BT_GATTC_ENABLE", True)
    add_idf_sdkconfig_option("CONFIG_BT_GATTS_ENABLE", True)
    # SSE drains/close requests run on HTTPD's task. A full work mailbox must
    # fail promptly for retry rather than block that same task waiting on itself.
    add_idf_sdkconfig_option("CONFIG_HTTPD_QUEUE_WORK_BLOCKING", False)
    # Cloud sync uses a verified HTTPS connection on a background task.
    if hasattr(esp32, "request_http_client"):
        esp32.request_http_client()
    else:
        # ESPHome 2026.9 uses explicit built-in component inclusion.
        esp32.include_builtin_idf_component("esp_http_client")
        esp32.include_builtin_idf_component("esp-tls")
    esp32.require_full_certificate_bundle()
    add_idf_sdkconfig_option("CONFIG_ESP_TLS_INSECURE", False)
    add_idf_sdkconfig_option("CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY", False)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_HAVE_TIME_DATE", True)

    # Passphrase is now optional - stored in NVS instead of config
    if CONF_PASSPHRASE in config:
        cg.add(var.set_passphrase(config[CONF_PASSPHRASE]))

    cg.add(var.set_single_device(config[CONF_SINGLE_DEVICE]))

    # Mesh libraries (avionmesh-cpp with recsrmesh-cpp + micro-ecc) as a
    # converted library: ESPHome's native ESP-IDF toolchain turns the
    # library.json manifest into an IDF component. No PlatformIO involved.
    cg.add_library("avionmesh", None, "https://github.com/oyvindkinsey/avionmesh-cpp.git#v1.1.3")

    # C++20 for the libraries
    cg.add_build_flag("-std=gnu++20")
    esp32_ble.register_gap_event_handler(parent, var)
    esp32_ble.register_gap_scan_event_handler(parent, var)
    esp32_ble.register_gattc_event_handler(parent, var)

    # Gzip web assets → C headers
    comp_dir = os.path.dirname(__file__)

    def _embed_asset_bytes(raw, header_name, symbol):
        hdr = os.path.join(comp_dir, header_name)
        compressed = gzip.compress(raw, compresslevel=9)
        hex_lines = []
        for i in range(0, len(compressed), 16):
            chunk = compressed[i : i + 16]
            hex_lines.append(", ".join(f"0x{b:02x}" for b in chunk))
        with open(hdr, "w") as f:
            f.write("#pragma once\n")
            f.write("#include <cstdint>\n")
            f.write("#include <cstddef>\n\n")
            f.write("// Auto-generated — do not edit\n")
            f.write(f"static const uint8_t {symbol}[] = {{\n")
            f.write(",\n".join(f"    {line}" for line in hex_lines))
            f.write("\n};\n\n")
            f.write(f"static const size_t {symbol}_SIZE = {len(compressed)};\n")

    # Inline CSS + JS into HTML so the UI loads in a single request
    with open(os.path.join(comp_dir, "web.html"), "r") as f:
        html = f.read()
    with open(os.path.join(comp_dir, "web.css"), "r") as f:
        css = f.read()
    with open(os.path.join(comp_dir, "web.js"), "r") as f:
        js = f.read()
    html = html.replace('<link rel="stylesheet" href="/ui.css">', f"<style>{css}</style>")
    html = html.replace('<script src="/ui.js"></script>', f"<script>{js}</script>")
    _embed_asset_bytes(html.encode("utf-8"), "web_content.h", "AVIONMESH_WEB_HTML")
