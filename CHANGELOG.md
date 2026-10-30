# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Unassociated scan decodes vendor/product codes from the device UUID
  announce and the Web UI preselects the device type at claim time
  (verified against the cloud product catalog).
- `scan_unassoc` events now report `devices[]` with `uuid_hash`,
  `vendor_code`, and `product_code`; `discover_mesh` entries carry a
  `short` flag for truncated ping replies.

### Changed

- Move to ESPHome's native ESP-IDF toolchain (ESPHome >= 2026.9.0).
  Mesh libraries (avionmesh-cpp v1.1.3 with recsrmesh-cpp v1.1.3) are
  consumed as a converted IDF library; the PlatformIO `lib_deps` path
  and `fix_cmake.py` are gone. Works on ESP-IDF 5.5 (Mbed TLS 3) and
  ESP-IDF 6 (Mbed TLS 4) via the upstream crypto backend.

### Fixed

- Read POST bodies via `handleBody()` for ESPHome >= 2026.8, where
  `web_server_idf` consumes the body before `handleRequest()` runs
  (originally contributed by @bdeboer-x, with explicit empty-body
  handling added on top).
- Replace `request->url()` with `url_to()` (removed API) and the
  `mbedtls/base64.h` dependency (removed in Mbed TLS 4) with a local
  base64 implementation.
- Stop reading past the end of short ping replies during mesh discovery;
  short replies are flagged and their fw/vendor/product fields stay zero
  instead of showing garbage.
- Bulk import batches NVS writes into a single commit and feeds the task
  watchdog — large imports previously rebooted the device mid-way and
  lost trailing groups.

## [1.0.0] - 2026-10-18

### Added

- clang-format / CMake build tooling, CI, and contributor templates.
