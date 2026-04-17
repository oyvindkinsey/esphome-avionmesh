#pragma once
#include <cstdint>

// ---- Minimal ESP-IDF BLE type stubs ----

typedef uint8_t esp_bd_addr_t[6];
typedef uint8_t esp_gatt_if_t;

enum esp_gap_ble_cb_event_t {
    ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT = 0,
    ESP_GAP_BLE_SCAN_RESULT_EVT,
};

enum esp_gattc_cb_event_t {
    ESP_GATTC_REG_EVT = 0,
    ESP_GATTC_OPEN_EVT,
    ESP_GATTC_SEARCH_CMPL_EVT,
    ESP_GATTC_REG_FOR_NOTIFY_EVT,
    ESP_GATTC_NOTIFY_EVT,
    ESP_GATTC_DISCONNECT_EVT,
};

enum esp_gatt_status_t { ESP_GATT_OK = 0 };
enum esp_bt_status_t   { ESP_BT_STATUS_SUCCESS = 0 };
enum esp_gatt_write_type_t { ESP_GATT_WRITE_TYPE_RSP = 0, ESP_GATT_WRITE_TYPE_NO_RSP };
enum esp_gatt_auth_req_t   { ESP_GATT_AUTH_REQ_NONE = 0 };
enum ble_scan_type_t   { BLE_SCAN_TYPE_ACTIVE = 0 };
enum ble_addr_type_t   { BLE_ADDR_TYPE_PUBLIC = 0 };
enum ble_scan_filter_t { BLE_SCAN_FILTER_ALLOW_ALL = 0 };
enum ble_scan_duplicate_t { BLE_SCAN_DUPLICATE_DISABLE = 0 };

static constexpr int ESP_OK = 0;
static constexpr int ESP_UUID_LEN_16  = 2;
static constexpr int ESP_UUID_LEN_128 = 16;
static constexpr uint16_t ESP_GATT_UUID_CHAR_CLIENT_CONFIG = 0x2902;
static constexpr int ESP_GAP_SEARCH_INQ_CMPL_EVT = 1;
static constexpr int ESP_GAP_SEARCH_INQ_RES_EVT  = 2;

union esp_ble_gap_cb_param_t {
    struct { int status; } scan_param_cmpl;
    struct { int status; uint16_t conn_id; } open;
    struct { int reason; } disconnect;
};

union esp_bt_uuid_t_u { uint16_t uuid16; uint8_t uuid128[16]; };
struct esp_bt_uuid_t { int len; esp_bt_uuid_t_u uuid; };
struct esp_gattc_char_elem_t  { uint16_t char_handle; };
struct esp_gattc_descr_elem_t { uint16_t handle; };

union esp_ble_gattc_cb_param_t {
    struct { int status; int app_id; }    reg;
    struct { int status; uint16_t conn_id; } open;
    struct { uint16_t handle; int status; } reg_for_notify;
    struct { uint16_t handle; uint16_t value_len; uint8_t *value; } notify;
    struct { int reason; } disconnect;
};

struct esp_ble_scan_params_t {
    ble_scan_type_t   scan_type;
    ble_addr_type_t   own_addr_type;
    ble_scan_filter_t scan_filter_policy;
    uint16_t          scan_interval;
    uint16_t          scan_window;
    ble_scan_duplicate_t scan_duplicate;
};

inline int esp_ble_gap_set_scan_params(esp_ble_scan_params_t *) { return 0; }
inline int esp_ble_gap_start_scanning(uint32_t) { return 0; }
inline int esp_ble_gattc_app_register(uint16_t) { return 0; }
inline int esp_ble_gattc_open(esp_gatt_if_t, esp_bd_addr_t, ble_addr_type_t, bool) { return 0; }
inline int esp_ble_gattc_search_service(esp_gatt_if_t, uint16_t, void *) { return 0; }
inline int esp_ble_gattc_get_char_by_uuid(esp_gatt_if_t, uint16_t, uint16_t, uint16_t,
                                           esp_bt_uuid_t, esp_gattc_char_elem_t *, uint16_t *) { return 1; }
inline int esp_ble_gattc_register_for_notify(esp_gatt_if_t, esp_bd_addr_t, uint16_t) { return 0; }
inline int esp_ble_gattc_get_descr_by_char_handle(esp_gatt_if_t, uint16_t, uint16_t,
                                                    esp_bt_uuid_t, esp_gattc_descr_elem_t *,
                                                    uint16_t *) { return 1; }
inline int esp_ble_gattc_write_char_descr(esp_gatt_if_t, uint16_t, uint16_t, uint16_t,
                                           uint8_t *, esp_gatt_write_type_t,
                                           esp_gatt_auth_req_t) { return 0; }
inline int esp_ble_gattc_close(esp_gatt_if_t, uint16_t) { return 0; }
inline int esp_ble_gattc_write_char(esp_gatt_if_t, uint16_t, uint16_t, uint16_t,
                                     uint8_t *, esp_gatt_write_type_t,
                                     esp_gatt_auth_req_t) { return 0; }

namespace esphome {
namespace esp32_ble {

struct BLEScanResult;

struct BLEGlobal {
    bool is_active() { return false; }
};
extern BLEGlobal *global_ble;

}  // namespace esp32_ble
}  // namespace esphome
