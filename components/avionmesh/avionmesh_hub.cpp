#include "avionmesh_hub.h"
#include "avionmesh_web.h"

#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/components/mqtt/mqtt_client.h"
#include "esphome/components/json/json_util.h"  // management commands still use JSON
#include "esphome/components/web_server_base/web_server_base.h"

#include <cstring>
#include <ctime>

static const char *TAG = "avionmesh";

static constexpr uint16_t CSRMESH_SERVICE_UUID16 = 0xFEF1;

/* CSRMesh characteristic UUIDs (128-bit) */
static const uint8_t CHAR_LOW_UUID128[16] = {
    0x00, 0x0b, 0x00, 0x5b, 0x02, 0x00, 0x03, 0x80,
    0xe3, 0x11, 0xaf, 0x9d, 0x00, 0xc0, 0xed, 0xc4
};
static const uint8_t CHAR_HIGH_UUID128[16] = {
    0x00, 0x0b, 0x00, 0x5b, 0x02, 0x00, 0x04, 0x80,
    0xe3, 0x11, 0xaf, 0x9d, 0x00, 0xc0, 0xed, 0xc4
};

namespace avionmesh {

float AvionMeshHub::get_setup_priority() const {
    return esphome::setup_priority::AFTER_BLUETOOTH;
}

void AvionMeshHub::setup() {
    ESP_LOGI(TAG, "Setting up AvionMesh hub...");

    db_.load();

    /* If passphrase was provided in YAML and not yet in NVS, copy it */
    if (!passphrase_.empty() && db_.passphrase().empty()) {
        ESP_LOGI(TAG, "Initializing passphrase from YAML config");
        db_.set_passphrase(passphrase_);
    }

    auto *mqtt = esphome::mqtt::global_mqtt_client;
    if (mqtt) {
        discovery_.set_node_name(esphome::App.get_name());
        discovery_.set_topic_prefix(mqtt->get_topic_prefix());
    }

    /* Initialize csrmesh crypto if passphrase exists */
    if (!db_.passphrase().empty()) {
        if (!init_crypto()) {
            ESP_LOGE(TAG, "csrmesh crypto initialization failed");
            return;
        }
    } else {
        ESP_LOGW(TAG, "No passphrase set - mesh operations disabled until passphrase is configured");
    }

    /* Daily time sync */
    this->set_interval("time_sync", 24 * 60 * 60 * 1000, [this]() {
        sync_time();
    });

    /* Re-publish discovery when HA comes online */
    if (mqtt) {
        mqtt->subscribe("homeassistant/status",
                        [this](const std::string &topic, const std::string &payload) {
                            if (payload == "online") {
                                ESP_LOGI(TAG, "HA online, re-publishing discovery");
                                publish_all_discovery();
                            }
                        }, 0);
    }

    /* Register web UI handler — deferred to loop() after web server is ready */

    ESP_LOGI(TAG, "AvionMesh hub initialized with %zu devices, %zu groups",
             db_.devices().size(), db_.groups().size());
}

void AvionMeshHub::dump_config() {
    ESP_LOGCONFIG(TAG, "AvionMesh Hub:");
    ESP_LOGCONFIG(TAG, "  Mesh initialized: %s", mesh_initialized_ ? "YES" : "NO");
    ESP_LOGCONFIG(TAG, "  Passphrase configured: %s", db_.passphrase().empty() ? "NO" : "YES");
    ESP_LOGCONFIG(TAG, "  BLE state: %u", static_cast<uint8_t>(ble_state_));
    ESP_LOGCONFIG(TAG, "  BLE char LOW: 0x%04X  HIGH: 0x%04X", char_low_handle_, char_high_handle_);
    ESP_LOGCONFIG(TAG, "  MQTT subscribed: %s", mqtt_subscribed_ ? "YES" : "NO");
    ESP_LOGCONFIG(TAG, "  Devices: %zu  Groups: %zu", db_.devices().size(), db_.groups().size());
}

/* ---- GAP event handler (dispatched by esp32_ble) ---- */

void AvionMeshHub::gap_event_handler(esp_gap_ble_cb_event_t event,
                                      esp_ble_gap_cb_param_t *param) {
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        if (param->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            esp_ble_gap_start_scanning(SCAN_WINDOW_MS / 1000);
        } else {
            ESP_LOGE(TAG, "Scan param set failed: %d", param->scan_param_cmpl.status);
            ble_state_ = BleState::Disconnected;
            reconnect_at_ms_ = esphome::millis() + RECONNECT_DELAY_MS;
        }
        break;

    default:
        break;
    }
}

void AvionMeshHub::gap_scan_event_handler(
        const esphome::esp32_ble::BLEScanResult &result) {
    if (result.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
        ESP_LOGD(TAG, "Scan complete");
        stop_scan_and_connect();
        return;
    }

    if (result.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT)
        return;
    if (ble_state_ != BleState::Scanning)
        return;

    /* Parse advertisement data for 0xFEF1 service UUID */
    const uint8_t *adv = result.ble_adv;
    uint8_t adv_len = result.adv_data_len + result.scan_rsp_len;

    size_t offset = 0;
    while (offset < adv_len) {
        uint8_t field_len = adv[offset];
        if (field_len == 0 || offset + field_len >= adv_len)
            break;

        uint8_t field_type = adv[offset + 1];
        const uint8_t *field_data = &adv[offset + 2];
        uint8_t data_len = field_len - 1;

        /* Complete/Incomplete 16-bit Service UUIDs */
        if ((field_type == 0x02 || field_type == 0x03) && data_len >= 2) {
            for (uint8_t i = 0; i + 1 < data_len; i += 2) {
                uint16_t uuid16 = field_data[i] | (field_data[i + 1] << 8);
                if (uuid16 == CSRMESH_SERVICE_UUID16) {
                    char addr_str[18];
                    snprintf(addr_str, sizeof(addr_str),
                             "%02X:%02X:%02X:%02X:%02X:%02X",
                             result.bda[0], result.bda[1], result.bda[2],
                             result.bda[3], result.bda[4], result.bda[5]);
                    ESP_LOGD(TAG, "CSRMesh bridge: %s RSSI=%d", addr_str, result.rssi);

                    if (result.rssi > best_rssi_) {
                        best_rssi_ = result.rssi;
                        std::memcpy(bridge_bda_, result.bda, 6);
                    }
                    return;
                }
            }
        }
        offset += field_len + 1;
    }
}

/* ---- GAP scanning ---- */

void AvionMeshHub::start_scan() {
    ble_state_ = BleState::Scanning;
    best_rssi_ = -999;
    std::memset(bridge_bda_, 0, sizeof(bridge_bda_));
    scan_start_ms_ = esphome::millis();
    ESP_LOGI(TAG, "Scanning for CSRMesh bridges...");

    esp_ble_scan_params_t scan_params = {};
    scan_params.scan_type = BLE_SCAN_TYPE_ACTIVE;
    scan_params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    scan_params.scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
    scan_params.scan_interval = 0x50;  // 50ms
    scan_params.scan_window = 0x30;    // 30ms
    scan_params.scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE;

    esp_ble_gap_set_scan_params(&scan_params);
}

void AvionMeshHub::stop_scan_and_connect() {
    if (ble_state_ != BleState::Scanning)
        return;
    connect_to_best();
}

void AvionMeshHub::connect_to_best() {
    if (best_rssi_ == -999) {
        ESP_LOGW(TAG, "No CSRMesh bridges found, retrying in %ums", RECONNECT_DELAY_MS);
        ble_state_ = BleState::Disconnected;
        reconnect_at_ms_ = esphome::millis() + RECONNECT_DELAY_MS;
        return;
    }

    char addr_str[18];
    snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             bridge_bda_[0], bridge_bda_[1], bridge_bda_[2],
             bridge_bda_[3], bridge_bda_[4], bridge_bda_[5]);
    ESP_LOGI(TAG, "Connecting to best bridge: %s (RSSI=%d)", addr_str, best_rssi_);

    ble_state_ = BleState::Connecting;
    esp_ble_gattc_open(gattc_if_, bridge_bda_, BLE_ADDR_TYPE_PUBLIC, true);
}

/* ---- GATTC event handler (dispatched by esp32_ble) ---- */

void AvionMeshHub::gattc_event_handler(esp_gattc_cb_event_t event,
                                        esp_gatt_if_t gattc_if,
                                        esp_ble_gattc_cb_param_t *param) {
    switch (event) {
    case ESP_GATTC_REG_EVT:
        if (param->reg.status == ESP_GATT_OK) {
            gattc_if_ = gattc_if;
            gattc_registered_ = true;
            app_id_ = param->reg.app_id;
            ESP_LOGI(TAG, "GATTC registered, starting scan");
            start_scan();
        }
        break;

    case ESP_GATTC_OPEN_EVT:
        if (param->open.status == ESP_GATT_OK) {
            on_connected(gattc_if, param->open.conn_id);
        } else {
            ESP_LOGW(TAG, "BLE connection failed: %d, will retry", param->open.status);
            on_disconnected();
        }
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT:
        on_service_discovery_complete();
        break;

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
        ESP_LOGI(TAG, "REG_FOR_NOTIFY handle=0x%04X status=%d",
                 param->reg_for_notify.handle, param->reg_for_notify.status);
        if (param->reg_for_notify.status != ESP_GATT_OK)
            break;

        /* Write CCCD to actually enable notifications on the remote device */
        esp_bt_uuid_t cccd_uuid = {.len = ESP_UUID_LEN_16, .uuid = {.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG}};
        esp_gattc_descr_elem_t desc_result;
        uint16_t count = 1;
        auto err = esp_ble_gattc_get_descr_by_char_handle(
            gattc_if, conn_id_, param->reg_for_notify.handle, cccd_uuid, &desc_result, &count);
        if (err != ESP_GATT_OK || count == 0) {
            ESP_LOGW(TAG, "CCCD not found for handle 0x%04X (err=%d)",
                     param->reg_for_notify.handle, err);
            break;
        }

        uint16_t notify_en = 1;
        esp_ble_gattc_write_char_descr(gattc_if, conn_id_, desc_result.handle,
                                        sizeof(notify_en), (uint8_t *)&notify_en,
                                        ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
        ESP_LOGI(TAG, "Wrote CCCD for handle 0x%04X (desc=0x%04X)",
                 param->reg_for_notify.handle, desc_result.handle);
        break;
    }

    case ESP_GATTC_NOTIFY_EVT: {
        ESP_LOGD(TAG, "NOTIFY handle=0x%04X len=%u",
                 param->notify.handle, param->notify.value_len);

        if (!mesh_initialized_ || ble_state_ != BleState::Ready)
            break;

        uint16_t handle = param->notify.handle;
        csrmesh::Characteristic ch;
        if (handle == char_low_handle_)
            ch = csrmesh::Characteristic::Low;
        else if (handle == char_high_handle_)
            ch = csrmesh::Characteristic::High;
        else
            break;

        csrmesh::feed_notify(mesh_ctx_, ch,
                              param->notify.value, param->notify.value_len,
                              esphome::millis());
        break;
    }

    case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGW(TAG, "BLE disconnected (reason=%d)", param->disconnect.reason);
        on_disconnected();
        break;

    default:
        break;
    }
}

void AvionMeshHub::on_connected(esp_gatt_if_t gattc_if, uint16_t conn_id) {
    conn_id_ = conn_id;
    gattc_if_ = gattc_if;
    ble_state_ = BleState::Discovering;
    ESP_LOGI(TAG, "BLE connected, discovering services...");
    esp_ble_gattc_search_service(gattc_if, conn_id, nullptr);
}

void AvionMeshHub::on_service_discovery_complete() {
    uint16_t count = 0;
    esp_gattc_char_elem_t result;

    esp_bt_uuid_t low_uuid;
    low_uuid.len = ESP_UUID_LEN_128;
    std::memcpy(low_uuid.uuid.uuid128, CHAR_LOW_UUID128, 16);
    count = 1;
    if (esp_ble_gattc_get_char_by_uuid(gattc_if_, conn_id_, 0x0001, 0xFFFF,
                                         low_uuid, &result, &count) == ESP_OK && count > 0) {
        char_low_handle_ = result.char_handle;
    }

    esp_bt_uuid_t high_uuid;
    high_uuid.len = ESP_UUID_LEN_128;
    std::memcpy(high_uuid.uuid.uuid128, CHAR_HIGH_UUID128, 16);
    count = 1;
    if (esp_ble_gattc_get_char_by_uuid(gattc_if_, conn_id_, 0x0001, 0xFFFF,
                                         high_uuid, &result, &count) == ESP_OK && count > 0) {
        char_high_handle_ = result.char_handle;
    }

    if (char_low_handle_ && char_high_handle_) {
        ESP_LOGI(TAG, "Found characteristics: LOW=0x%04X HIGH=0x%04X",
                 char_low_handle_, char_high_handle_);

        esp_ble_gattc_register_for_notify(gattc_if_, bridge_bda_, char_low_handle_);
        esp_ble_gattc_register_for_notify(gattc_if_, bridge_bda_, char_high_handle_);

        ble_state_ = BleState::Ready;
        /* Mesh is now fully operational (crypto + BLE connected) */
        mesh_initialized_ = true;
        ESP_LOGI(TAG, "BLE ready - mesh operational");
    } else {
        ESP_LOGE(TAG, "CSRMesh characteristics not found (LOW=0x%04X HIGH=0x%04X)",
                 char_low_handle_, char_high_handle_);
        esp_ble_gattc_close(gattc_if_, conn_id_);
        on_disconnected();
    }
}

void AvionMeshHub::on_disconnected() {
    char_low_handle_ = 0;
    char_high_handle_ = 0;
    mqtt_subscribed_ = false;
    initial_read_done_ = false;
    time_synced_ = false;

    if (associating_) {
        associating_ = false;
        csrmesh::associate_cancel(mesh_ctx_);
    }

    ble_state_ = BleState::Disconnected;
    reconnect_at_ms_ = esphome::millis() + RECONNECT_DELAY_MS;
    update_mesh_initialized();
    ESP_LOGI(TAG, "Will reconnect in %ums", RECONNECT_DELAY_MS);
}

/* ---- Crypto initialization ---- */

bool AvionMeshHub::init_crypto() {
    if (db_.passphrase().empty()) {
        ESP_LOGW(TAG, "Cannot init crypto: no passphrase");
        return false;
    }

    auto ble_write_fn = [this](csrmesh::Characteristic ch, const uint8_t *data,
                               size_t len, bool response) -> int {
        return ble_write(ch, data, len, response);
    };

    auto err = csrmesh::init(mesh_ctx_, ble_write_fn, db_.passphrase().c_str());
    if (err != csrmesh::Error::Ok) {
        ESP_LOGE(TAG, "csrmesh::init failed: %d", static_cast<int>(err));
        return false;
    }

    csrmesh::set_rx_callback(mesh_ctx_, [this](uint16_t mcp_source, uint16_t crypto_source,
                                                uint8_t opcode, const uint8_t *payload,
                                                size_t payload_len) {
        on_mesh_rx(mcp_source, crypto_source, opcode, payload, payload_len);
    });

    crypto_initialized_ = true;
    ESP_LOGI(TAG, "CSRMesh crypto initialized");
    update_mesh_initialized();
    return true;
}

void AvionMeshHub::update_mesh_initialized() {
    bool was_initialized = mesh_initialized_;
    mesh_initialized_ = crypto_initialized_ && (ble_state_ == BleState::Ready);

    if (mesh_initialized_ && !was_initialized) {
        ESP_LOGI(TAG, "Mesh is now fully operational");
    } else if (!mesh_initialized_ && was_initialized) {
        ESP_LOGI(TAG, "Mesh is no longer operational (crypto=%d, ble=%d)",
                 crypto_initialized_, static_cast<int>(ble_state_));
    }

    if (mesh_initialized_ != was_initialized && web_handler_) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "{\"ble_state\":%u,\"mesh_initialized\":%s,\"rx_count\":%u}",
                 static_cast<uint8_t>(ble_state_),
                 mesh_initialized_ ? "true" : "false",
                 rx_count_);
        web_handler_->send_event("meta", buf);
    }
}

/* ---- Main loop ---- */

void AvionMeshHub::loop() {
    if (!web_registered_) {
        auto *web_base = esphome::web_server_base::global_web_server_base;
        if (web_base) {
            web_handler_ = new AvionMeshWebHandler(this);
            web_base->add_handler(web_handler_);
            web_registered_ = true;
            ESP_LOGI(TAG, "Web UI registered at /avionmesh");
        }
    }

    if (web_handler_)
        web_handler_->sse_loop();

    /* Process deferred web requests on the main loop */
    if (pending_discover_mesh_) {
        pending_discover_mesh_ = false;
        handle_discover_mesh();
    }
    if (pending_scan_unassoc_) {
        pending_scan_unassoc_ = false;
        handle_scan_unassociated();
    }
    if (pending_examine_) {
        pending_examine_ = false;
        handle_examine_device(pending_examine_id_);
    }
    if (pending_claim_auto_) {
        pending_claim_auto_ = false;
        handle_claim_device_auto();
    }

    process_deferred_actions();

    /* Defer GATTC registration until esp32_ble has fully initialized BLE */
    if (!gattc_registered_ && esphome::esp32_ble::global_ble->is_active()) {
        esp_ble_gattc_app_register(0);
    }

    if (ble_state_ == BleState::Disconnected &&
        esphome::millis() >= reconnect_at_ms_) {
        start_scan();
    }

    if (!mgmt_subscribed_) {
        auto *mqtt = esphome::mqtt::global_mqtt_client;
        if (mqtt && mqtt->is_connected()) {
            ESP_LOGI(TAG, "MQTT connected, subscribing to management commands");
            mqtt->subscribe(discovery_.management_command_topic(),
                            [this](const std::string &topic, const std::string &payload) {
                                on_mqtt_command(payload);
                            }, 0);
            mgmt_subscribed_ = true;
        }
    }

    if (!mqtt_subscribed_ && ble_state_ == BleState::Ready) {
        auto *mqtt = esphome::mqtt::global_mqtt_client;
        if (mqtt && mqtt->is_connected()) {
            ESP_LOGI(TAG, "BLE ready, publishing discovery and subscribing light commands");
            publish_all_discovery();
            subscribe_all_commands();
        }
    }

    if (mqtt_subscribed_ && !initial_read_done_) {
        initial_read_done_ = true;
        this->set_timeout("initial_read", 2000, [this]() {
            read_all_dimming();
            this->set_timeout("initial_color_read", 1000, [this]() {
                read_all_color();
            });
        });
    }

    if (mqtt_subscribed_ && !time_synced_) {
        time_synced_ = true;
        this->set_timeout("initial_time_sync", 5000, [this]() {
            sync_time();
        });
    }

    csrmesh::poll(mesh_ctx_, esphome::millis());

    if (associating_) {
        if (csrmesh::protocol::is_complete(proto_ctx_)) {
            ESP_LOGI(TAG, "Association complete for device %u", proto_ctx_.device_id);
            associating_ = false;
            csrmesh::protocol::cleanup(proto_ctx_);
            send_response("{\"action\":\"claim_device\",\"status\":\"ok\"}");
        } else if (csrmesh::protocol::is_error(proto_ctx_)) {
            ESP_LOGE(TAG, "Association failed: %s", proto_ctx_.error);
            associating_ = false;
            csrmesh::protocol::cleanup(proto_ctx_);
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "{\"action\":\"claim_device\",\"status\":\"error\",\"message\":\"%s\"}",
                     proto_ctx_.error ? proto_ctx_.error : "unknown");
            send_response(buf);
        } else if (esphome::millis() - association_start_ms_ > ASSOCIATION_TIMEOUT_MS) {
            ESP_LOGE(TAG, "Association timed out (state=%s)",
                     csrmesh::protocol::state_name(proto_ctx_.state));
            associating_ = false;
            csrmesh::associate_cancel(mesh_ctx_);
            send_response("{\"action\":\"claim_device\",\"status\":\"error\",\"message\":\"timeout\"}");
        }
    }
}

/* ---- Deferred action processing ---- */

void AvionMeshHub::process_deferred_actions() {
    std::vector<DeferredAction> actions;
    {
        std::lock_guard<std::mutex> lock(action_mutex_);
        if (pending_actions_.empty())
            return;
        actions.swap(pending_actions_);
    }

    for (auto &act : actions) {
        switch (act.type) {
        case DeferredAction::Control:
            if (act.brightness >= 0) {
                Command cmd;
                cmd_brightness(act.id1, static_cast<uint8_t>(act.brightness), cmd);
                send_cmd(mesh_ctx_, cmd);
                auto &state = device_states_[act.id1];
                state.brightness = static_cast<uint8_t>(act.brightness);
                state.brightness_known = true;
                publish_device_state(act.id1);
            }
            if (act.color_temp > 0) {
                Command cmd;
                cmd_color_temp(act.id1, static_cast<uint16_t>(act.color_temp), cmd);
                send_cmd(mesh_ctx_, cmd);
                auto &state = device_states_[act.id1];
                state.color_temp = static_cast<uint16_t>(act.color_temp);
                state.color_temp_known = true;
                publish_device_state(act.id1);
            }
            break;

        case DeferredAction::AddDiscovered:
            handle_add_discovered(act.id1, act.name, act.product_type);
            if (web_handler_) {
                auto *dev = db_.find_device(act.id1);
                if (dev) {
                    std::string json = "{\"avion_id\":";
                    json += std::to_string(dev->avion_id);
                    json += ",\"name\":\"";
                    json += dev->name;
                    json += "\",\"product_type\":";
                    json += std::to_string(dev->product_type);
                    json += ",\"product_name\":\"";
                    json += product_name(dev->product_type);
                    json += "\",\"groups\":[]}";
                    web_handler_->send_event("device_added", json);
                }
            }
            break;

        case DeferredAction::UnclaimDevice:
            handle_unclaim_device(act.id1);
            if (web_handler_) {
                char buf[64];
                snprintf(buf, sizeof(buf), "{\"avion_id\":%u}", act.id1);
                web_handler_->send_event("device_removed", buf);
            }
            break;

        case DeferredAction::CreateGroup: {
            uint16_t group_id = next_group_id();
            if (group_id != 0) {
                handle_create_group(group_id, act.name);
                if (web_handler_) {
                    std::string json = "{\"group_id\":";
                    json += std::to_string(group_id);
                    json += ",\"name\":\"";
                    json += act.name;
                    json += "\",\"members\":[]}";
                    web_handler_->send_event("group_added", json);
                }
            }
            break;
        }

        case DeferredAction::DeleteGroup:
            handle_delete_group(act.id1);
            if (web_handler_) {
                char buf[64];
                snprintf(buf, sizeof(buf), "{\"group_id\":%u}", act.id1);
                web_handler_->send_event("group_removed", buf);
            }
            break;

        case DeferredAction::AddToGroup:
            handle_add_to_group(act.id1, act.id2);
            if (web_handler_) {
                auto *grp = db_.find_group(act.id2);
                if (grp) {
                    std::string json = "{\"group_id\":";
                    json += std::to_string(grp->group_id);
                    json += ",\"name\":\"";
                    json += grp->name;
                    json += "\",\"members\":[";
                    for (size_t i = 0; i < grp->member_ids.size(); i++) {
                        if (i > 0) json += ",";
                        json += std::to_string(grp->member_ids[i]);
                    }
                    json += "]}";
                    web_handler_->send_event("group_updated", json);
                }
            }
            break;

        case DeferredAction::RemoveFromGroup:
            handle_remove_from_group(act.id1, act.id2);
            if (web_handler_) {
                auto *grp = db_.find_group(act.id2);
                if (grp) {
                    std::string json = "{\"group_id\":";
                    json += std::to_string(grp->group_id);
                    json += ",\"name\":\"";
                    json += grp->name;
                    json += "\",\"members\":[";
                    for (size_t i = 0; i < grp->member_ids.size(); i++) {
                        if (i > 0) json += ",";
                        json += std::to_string(grp->member_ids[i]);
                    }
                    json += "]}";
                    web_handler_->send_event("group_updated", json);
                }
            }
            break;

        case DeferredAction::Import: {
            int added_devices = 0, added_groups = 0;
            esphome::json::parse_json(act.body, [&](JsonObject root) -> bool {
                if (root["reset"] | false) {
                    ESP_LOGI(TAG, "Import with reset: clearing existing data");
                    for (auto &dev : db_.devices())
                        discovery_.remove_light(dev.avion_id);
                    for (auto &grp : db_.groups())
                        discovery_.remove_light(grp.group_id);
                    db_.clear();
                    db_.load();
                    device_states_.clear();
                }

                if (root["passphrase"].is<const char *>()) {
                    std::string passphrase = root["passphrase"].as<std::string>();
                    ESP_LOGI(TAG, "Setting passphrase from import (len=%zu)", passphrase.size());
                    db_.set_passphrase(passphrase);
                    crypto_initialized_ = false;
                    mesh_initialized_ = false;
                    if (!init_crypto()) {
                        ESP_LOGE(TAG, "Failed to initialize crypto with imported passphrase");
                        return true;
                    }
                    ESP_LOGI(TAG, "Crypto initialized with imported passphrase");
                }

                JsonArray devices = root["devices"];
                for (JsonObject dev : devices) {
                    uint16_t device_id = dev["device_id"] | 0u;
                    std::string name = dev["name"] | "Unknown";
                    uint8_t product_type = dev["product_type"] | 0u;
                    if (device_id == 0) continue;
                    if (db_.find_device(device_id)) continue;

                    bool has_dim = has_dimming(product_type);
                    bool has_ct = has_color_temp(product_type);
                    db_.add_device(device_id, product_type, name);
                    discovery_.publish_light(device_id, name, has_dim, has_ct,
                                             product_name(product_type));
                    added_devices++;
                }

                JsonArray groups = root["groups"];
                for (JsonObject grp : groups) {
                    uint16_t group_id = grp["group_id"] | 0u;
                    std::string gname = grp["name"] | "Group";
                    if (group_id == 0) continue;
                    if (!db_.find_group(group_id)) {
                        db_.add_group(group_id, gname);
                        discovery_.publish_light(group_id, gname, true, true);
                        added_groups++;
                    }

                    JsonArray members = grp["members"];
                    for (JsonVariant m : members) {
                        uint16_t member_id = m.as<uint16_t>();
                        if (member_id > 0) {
                            db_.add_device_to_group(member_id, group_id);
                            Command cmd;
                            cmd_insert_group(member_id, group_id, cmd);
                            send_cmd(mesh_ctx_, cmd);
                        }
                    }
                }
                return true;
            });

            publish_all_discovery();
            subscribe_all_commands();

            if (web_handler_) {
                char buf[96];
                snprintf(buf, sizeof(buf),
                         "{\"added_devices\":%d,\"added_groups\":%d}",
                         added_devices, added_groups);
                web_handler_->send_event("import_result", buf);
                web_handler_->reset_sync();
            }
            break;
        }
        }
    }
}

/* ---- BLE write ---- */

int AvionMeshHub::ble_write(csrmesh::Characteristic ch, const uint8_t *data,
                              size_t len, bool response) {
    if (ble_state_ != BleState::Ready || !char_low_handle_ || !char_high_handle_)
        return -1;

    uint16_t handle = (ch == csrmesh::Characteristic::Low) ? char_low_handle_
                                                            : char_high_handle_;

    auto err = esp_ble_gattc_write_char(
        gattc_if_, conn_id_, handle,
        len, const_cast<uint8_t *>(data),
        response ? ESP_GATT_WRITE_TYPE_RSP : ESP_GATT_WRITE_TYPE_NO_RSP,
        ESP_GATT_AUTH_REQ_NONE);

    return (err == ESP_OK) ? 0 : -1;
}

/* ---- Mesh RX ---- */

void AvionMeshHub::on_mesh_rx(uint16_t mcp_source, uint16_t crypto_source,
                                uint8_t opcode, const uint8_t *payload,
                                size_t payload_len) {
    rx_count_++;
    uint16_t src = (mcp_source == 0x8000) ? crypto_source : mcp_source;
    ESP_LOGD(TAG, "RX #%u: src=%u opcode=0x%02X len=%zu", rx_count_, src, opcode, payload_len);

    if ((discovering_mesh_ || examining_) && opcode == MODEL_OPCODE &&
        payload_len >= 10 && payload[0] == static_cast<uint8_t>(Verb::Ping)) {
        uint16_t device_id = (mcp_source == 0x8000) ? crypto_source : mcp_source;

        /* Skip our own broadcast echo (mcp_source == 0 means message is from us) */
        if (discovering_mesh_ && mcp_source == 0)
            return;

        if (examining_ && device_id == examine_target_) {
            examining_ = false;

            char buf[256];
            snprintf(buf, sizeof(buf),
                     "{\"avion_id\":%u,\"fw\":\"%u.%u.%u\","
                     "\"flags\":%u,\"vendor_id\":%u,\"csr_product_id\":%u}",
                     device_id, payload[3], payload[4], payload[5],
                     payload[6], (payload[7] << 8) | payload[8], payload[9]);

            if (web_handler_)
                web_handler_->send_event("examine", buf);

            std::string mqtt_buf = "{\"action\":\"examine_device\",\"status\":\"ok\",";
            mqtt_buf += buf + 1;  // skip opening brace, append to mqtt prefix
            send_response(mqtt_buf);
        }

        if (discovering_mesh_) {
            bool seen = false;
            for (auto &d : discovered_devices_) {
                if (d.device_id == device_id) { seen = true; break; }
            }
            if (!seen) {
                /* Dump raw payload for diagnostics */
                if (web_handler_) {
                    char diag[256];
                    int pos = snprintf(diag, sizeof(diag),
                                       "{\"type\":\"ping_rx\",\"mcp_src\":%u,"
                                       "\"crypto_src\":%u,\"len\":%zu,\"bytes\":[",
                                       mcp_source, crypto_source, payload_len);
                    for (size_t b = 0; b < payload_len && b < 16; b++) {
                        pos += snprintf(diag + pos, sizeof(diag) - pos,
                                        "%s%u", b > 0 ? "," : "", payload[b]);
                    }
                    snprintf(diag + pos, sizeof(diag) - pos, "]}");
                    web_handler_->send_event("debug", diag);
                }

                DiscoveredDevice dev;
                dev.device_id = device_id;
                dev.fw_major = payload[3];
                dev.fw_minor = payload[4];
                dev.fw_patch = payload[5];
                dev.flags = payload[6];
                dev.vendor_id = (static_cast<uint16_t>(payload[7]) << 8) | payload[8];
                dev.csr_product_id = payload[9];
                discovered_devices_.push_back(dev);
                ESP_LOGI(TAG, "Discovered device %u: fw=%u.%u.%u product=%u",
                         device_id, dev.fw_major, dev.fw_minor, dev.fw_patch,
                         dev.csr_product_id);
            }
        }
    }

    Status status;
    if (!parse_response(mcp_source, crypto_source, opcode, payload, payload_len, status))
        return;

    auto *dev = db_.find_device(status.avid);
    if (!dev)
        return;

    auto &state = device_states_[status.avid];
    if (status.has_brightness) {
        state.brightness = status.brightness;
        state.brightness_known = true;
    }
    if (status.has_color_temp) {
        state.color_temp = status.color_temp;
        state.color_temp_known = true;
    }

    publish_device_state(status.avid);
}

/* ---- MQTT command handler ---- */

void AvionMeshHub::on_mqtt_command(const std::string &payload) {
    ESP_LOGI(TAG, "Management command: %s", payload.c_str());

    esphome::json::parse_json(payload, [this](JsonObject root) -> bool {
        std::string action = root["action"] | "";

        if (action == "status") {
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "{\"action\":\"status\",\"ble_state\":%u,\"devices\":%zu,\"groups\":%zu,\"rx_count\":%u}",
                     static_cast<uint8_t>(ble_state_), db_.devices().size(), db_.groups().size(), rx_count_);
            send_response(buf);
            return true;
        }

        if (ble_state_ != BleState::Ready) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "{\"action\":\"%s\",\"status\":\"error\",\"message\":\"ble_not_ready\",\"ble_state\":%u}",
                     action.c_str(), static_cast<uint8_t>(ble_state_));
            send_response(buf);
            return true;
        }

        if (action == "scan_unassociated") {
            handle_scan_unassociated();
        } else if (action == "claim_device") {
            handle_claim_device(
                root["uuid_hash"] | 0u,
                root["device_id"] | 0u,
                root["name"] | "Unknown",
                root["product_type"] | 0u);
        } else if (action == "unclaim_device") {
            handle_unclaim_device(root["avion_id"] | 0u);
        } else if (action == "create_group") {
            handle_create_group(root["group_id"] | 0u, root["name"] | "Group");
        } else if (action == "delete_group") {
            handle_delete_group(root["group_id"] | 0u);
        } else if (action == "add_to_group") {
            handle_add_to_group(root["avion_id"] | 0u, root["group_id"] | 0u);
        } else if (action == "remove_from_group") {
            handle_remove_from_group(root["avion_id"] | 0u, root["group_id"] | 0u);
        } else if (action == "discover_mesh") {
            handle_discover_mesh();
        } else if (action == "add_discovered") {
            handle_add_discovered(
                root["device_id"] | 0u,
                root["name"] | "Unknown",
                root["product_type"] | 0u);
        } else if (action == "examine_device") {
            handle_examine_device(root["avion_id"] | 0u);
        } else if (action == "set_mesh_brightness") {
            Command cmd;
            cmd_brightness(0, root["brightness"] | 0u, cmd);
            send_cmd(mesh_ctx_, cmd);
        } else if (action == "set_mesh_color_temp") {
            Command cmd;
            cmd_color_temp(0, root["kelvin"] | 3000u, cmd);
            send_cmd(mesh_ctx_, cmd);
        } else if (action == "sync_time") {
            sync_time();
        } else if (action == "read_all") {
            read_all_dimming();
        } else {
            ESP_LOGW(TAG, "Unknown action: %s", action.c_str());
            return false;
        }
        return true;
    });
}

void AvionMeshHub::handle_scan_unassociated() {
    ESP_LOGI(TAG, "Starting unassociated device scan...");

    scanning_unassociated_ = true;
    scan_uuid_hashes_.clear();

    csrmesh::discover_start(mesh_ctx_, [this](const uint8_t *uuid, size_t uuid_len,
                                               uint32_t uuid_hash) {
        scan_uuid_hashes_.push_back(uuid_hash);

        char buf[128];
        snprintf(buf, sizeof(buf),
                 "{\"action\":\"scan_unassociated\",\"uuid_hash\":\"0x%08x\"}", uuid_hash);
        send_response(buf);
    });

    this->set_timeout("scan_stop", 5000, [this]() {
        csrmesh::discover_stop(mesh_ctx_);

        std::string json = "{\"uuid_hashes\":[";
        for (size_t i = 0; i < scan_uuid_hashes_.size(); i++) {
            if (i > 0) json += ",";
            char buf[16];
            snprintf(buf, sizeof(buf), "\"0x%08x\"", scan_uuid_hashes_[i]);
            json += buf;
        }
        json += "]}";

        scanning_unassociated_ = false;

        if (web_handler_)
            web_handler_->send_event("scan_unassoc", json);

        send_response("{\"action\":\"scan_unassociated\",\"status\":\"done\"}");
    });
}

void AvionMeshHub::handle_claim_device(uint32_t uuid_hash, uint16_t device_id,
                                         const std::string &name, uint8_t product_type) {
    if (associating_) {
        send_response("{\"action\":\"claim_device\",\"status\":\"error\",\"message\":\"busy\"}");
        return;
    }

    ESP_LOGI(TAG, "Claiming device: uuid_hash=0x%08x, device_id=%u, name=%s",
             uuid_hash, device_id, name.c_str());

    auto err = csrmesh::protocol::init(proto_ctx_, uuid_hash, device_id,
                                        passphrase_.c_str());
    if (err != csrmesh::Error::Ok) {
        ESP_LOGE(TAG, "Proto init failed: %d", static_cast<int>(err));
        send_response("{\"action\":\"claim_device\",\"status\":\"error\",\"message\":\"proto_init\"}");
        return;
    }

    err = csrmesh::associate_start(mesh_ctx_, &proto_ctx_, uuid_hash, device_id);
    if (err != csrmesh::Error::Ok) {
        ESP_LOGE(TAG, "Associate start failed: %d", static_cast<int>(err));
        csrmesh::protocol::cleanup(proto_ctx_);
        send_response("{\"action\":\"claim_device\",\"status\":\"error\",\"message\":\"start_failed\"}");
        return;
    }

    associating_ = true;
    association_start_ms_ = esphome::millis();

    bool has_dim = has_dimming(product_type);
    bool has_ct = has_color_temp(product_type);
    db_.add_device(device_id, product_type, name);
    discovery_.publish_light(device_id, name, has_dim, has_ct, product_name(product_type));

    auto *mqtt_client = esphome::mqtt::global_mqtt_client;
    if (mqtt_client) {
        mqtt_client->subscribe(discovery_.command_topic(device_id),
                        [this, device_id](const std::string &, const std::string &payload) {
                            on_switch_command(device_id, payload);
                        }, 0);
        if (has_dim) {
            mqtt_client->subscribe(discovery_.brightness_command_topic(device_id),
                            [this, device_id](const std::string &, const std::string &payload) {
                                on_brightness_command(device_id, payload);
                            }, 0);
        }
        if (has_ct) {
            mqtt_client->subscribe(discovery_.color_temp_command_topic(device_id),
                            [this, device_id](const std::string &, const std::string &payload) {
                                on_color_temp_command(device_id, payload);
                            }, 0);
        }
    }
}

uint16_t AvionMeshHub::next_device_id() {
    for (uint16_t id = MIN_DEVICE_ID; id <= MAX_DEVICE_ID; ++id) {
        if (db_.find_device(id))
            continue;
        bool in_discovered = false;
        for (auto &d : discovered_devices_) {
            if (d.device_id == id) {
                in_discovered = true;
                break;
            }
        }
        if (!in_discovered)
            return id;
    }
    return 0;
}

uint16_t AvionMeshHub::next_group_id() {
    for (uint16_t id = MIN_GROUP_ID; id <= MAX_GROUP_ID; ++id) {
        if (!db_.find_group(id))
            return id;
    }
    return 0;
}

void AvionMeshHub::handle_claim_device_auto() {
    ESP_LOGI(TAG, "Starting auto-claim: mesh ping to verify available IDs");

    discovering_mesh_ = true;
    discovered_devices_.clear();

    Command cmd;
    cmd_ping(0, cmd);
    send_cmd(mesh_ctx_, cmd);

    this->set_timeout("auto_claim_scan", 5000, [this]() {
        discovering_mesh_ = false;

        uint16_t device_id = next_device_id();
        if (device_id == 0) {
            ESP_LOGE(TAG, "No available device IDs in range %u-%u", MIN_DEVICE_ID, MAX_DEVICE_ID);
            if (web_handler_)
                web_handler_->send_event("claim_result",
                    "{\"status\":\"error\",\"message\":\"no_available_ids\"}");
            return;
        }

        ESP_LOGI(TAG, "Auto-claim assigning device ID %u", device_id);
        handle_claim_device(pending_claim_uuid_hash_, device_id,
                           pending_claim_name_, pending_claim_product_type_);

        char buf[128];
        snprintf(buf, sizeof(buf),
                 "{\"status\":\"ok\",\"device_id\":%u}", device_id);
        if (web_handler_)
            web_handler_->send_event("claim_result", buf);
    });
}

void AvionMeshHub::handle_unclaim_device(uint16_t avion_id) {
    ESP_LOGI(TAG, "Unclaiming device %u", avion_id);

    if (associating_) {
        associating_ = false;
        csrmesh::associate_cancel(mesh_ctx_);
    }

    csrmesh::disassociate(mesh_ctx_, avion_id);
    db_.remove_device(avion_id);
    discovery_.remove_light(avion_id);

    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"action\":\"unclaim_device\",\"avion_id\":%u,\"status\":\"ok\"}", avion_id);
    send_response(buf);
}

void AvionMeshHub::handle_create_group(uint16_t group_id, const std::string &name) {
    db_.add_group(group_id, name);
    discovery_.publish_light(group_id, name, true, true);

    auto *mqtt_client = esphome::mqtt::global_mqtt_client;
    if (mqtt_client) {
        mqtt_client->subscribe(discovery_.command_topic(group_id),
                        [this, group_id](const std::string &, const std::string &payload) {
                            on_switch_command(group_id, payload);
                        }, 0);
        mqtt_client->subscribe(discovery_.brightness_command_topic(group_id),
                        [this, group_id](const std::string &, const std::string &payload) {
                            on_brightness_command(group_id, payload);
                        }, 0);
        mqtt_client->subscribe(discovery_.color_temp_command_topic(group_id),
                        [this, group_id](const std::string &, const std::string &payload) {
                            on_color_temp_command(group_id, payload);
                        }, 0);
    }

    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"action\":\"create_group\",\"group_id\":%u,\"status\":\"ok\"}", group_id);
    send_response(buf);
}

void AvionMeshHub::handle_delete_group(uint16_t group_id) {
    db_.remove_group(group_id);
    discovery_.remove_light(group_id);

    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"action\":\"delete_group\",\"group_id\":%u,\"status\":\"ok\"}", group_id);
    send_response(buf);
}

void AvionMeshHub::handle_add_to_group(uint16_t avion_id, uint16_t group_id) {
    Command cmd;
    cmd_insert_group(avion_id, group_id, cmd);
    send_cmd(mesh_ctx_, cmd);
    db_.add_device_to_group(avion_id, group_id);

    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"action\":\"add_to_group\",\"avion_id\":%u,\"group_id\":%u,\"status\":\"ok\"}",
             avion_id, group_id);
    send_response(buf);
}

void AvionMeshHub::handle_remove_from_group(uint16_t avion_id, uint16_t group_id) {
    Command cmd;
    cmd_delete_group(avion_id, group_id, cmd);
    send_cmd(mesh_ctx_, cmd);
    db_.remove_device_from_group(avion_id, group_id);

    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"action\":\"remove_from_group\",\"avion_id\":%u,\"group_id\":%u,\"status\":\"ok\"}",
             avion_id, group_id);
    send_response(buf);
}

void AvionMeshHub::handle_discover_mesh() {
    if (discovering_mesh_) {
        send_response("{\"action\":\"discover_mesh\",\"status\":\"error\",\"message\":\"busy\"}");
        return;
    }

    ESP_LOGI(TAG, "Starting mesh discovery (broadcast PING)...");
    discovering_mesh_ = true;
    discovered_devices_.clear();

    Command cmd;
    cmd_ping(0, cmd);
    send_cmd(mesh_ctx_, cmd);

    this->set_timeout("discover_stop", 5000, [this]() {
        std::string devices_arr = "[";
        for (size_t i = 0; i < discovered_devices_.size(); i++) {
            auto &d = discovered_devices_[i];
            bool known = db_.find_device(d.device_id) != nullptr;
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "%s{\"device_id\":%u,\"fw\":\"%u.%u.%u\","
                     "\"vendor_id\":%u,\"csr_product_id\":%u,\"known\":%s}",
                     i > 0 ? "," : "",
                     d.device_id, d.fw_major, d.fw_minor, d.fw_patch,
                     d.vendor_id, d.csr_product_id,
                     known ? "true" : "false");
            devices_arr += buf;
        }
        devices_arr += "]";

        discovering_mesh_ = false;

        ESP_LOGI(TAG, "Mesh discovery complete: %zu device(s) found",
                 discovered_devices_.size());

        if (web_handler_)
            web_handler_->send_event("discover_mesh",
                                     "{\"devices\":" + devices_arr + "}");

        send_response("{\"action\":\"discover_mesh\",\"status\":\"done\","
                      "\"devices\":" + devices_arr + "}");
    });
}

void AvionMeshHub::handle_add_discovered(uint16_t device_id, const std::string &name,
                                           uint8_t product_type) {
    if (db_.find_device(device_id)) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "{\"action\":\"add_discovered\",\"device_id\":%u,"
                 "\"status\":\"error\",\"message\":\"already_exists\"}", device_id);
        send_response(buf);
        return;
    }

    ESP_LOGI(TAG, "Adding discovered device: id=%u, name=%s, product_type=%u",
             device_id, name.c_str(), product_type);

    bool has_dim = has_dimming(product_type);
    bool has_ct = has_color_temp(product_type);
    db_.add_device(device_id, product_type, name);
    discovery_.publish_light(device_id, name, has_dim, has_ct, product_name(product_type));

    auto *mqtt_client = esphome::mqtt::global_mqtt_client;
    if (mqtt_client) {
        mqtt_client->subscribe(discovery_.command_topic(device_id),
                        [this, device_id](const std::string &, const std::string &payload) {
                            on_switch_command(device_id, payload);
                        }, 0);
        if (has_dim) {
            mqtt_client->subscribe(discovery_.brightness_command_topic(device_id),
                            [this, device_id](const std::string &, const std::string &payload) {
                                on_brightness_command(device_id, payload);
                            }, 0);
        }
        if (has_ct) {
            mqtt_client->subscribe(discovery_.color_temp_command_topic(device_id),
                            [this, device_id](const std::string &, const std::string &payload) {
                                on_color_temp_command(device_id, payload);
                            }, 0);
        }
    }

    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"action\":\"add_discovered\",\"device_id\":%u,\"status\":\"ok\"}", device_id);
    send_response(buf);
}

void AvionMeshHub::handle_examine_device(uint16_t avion_id) {
    if (examining_) {
        send_response("{\"action\":\"examine_device\",\"status\":\"error\",\"message\":\"busy\"}");
        return;
    }

    ESP_LOGI(TAG, "Examining device %u", avion_id);
    examining_ = true;
    examine_target_ = avion_id;

    Command cmd;
    cmd_ping(avion_id, cmd);
    send_cmd(mesh_ctx_, cmd);

    this->set_timeout("examine_timeout", 5000, [this]() {
        if (examining_) {
            examining_ = false;

            char buf[128];
            snprintf(buf, sizeof(buf),
                     "{\"avion_id\":%u,\"error\":\"timeout\"}", examine_target_);
            if (web_handler_)
                web_handler_->send_event("examine", buf);

            snprintf(buf, sizeof(buf),
                     "{\"action\":\"examine_device\",\"avion_id\":%u,"
                     "\"status\":\"error\",\"message\":\"timeout\"}", examine_target_);
            send_response(buf);
        }
    });
}

void AvionMeshHub::handle_set_passphrase(const std::string &passphrase) {
    ESP_LOGI(TAG, "Setting passphrase (length=%zu)", passphrase.size());
    db_.set_passphrase(passphrase);

    /* Reinitialize crypto with new passphrase */
    crypto_initialized_ = false;
    mesh_initialized_ = false;

    if (!init_crypto()) {
        ESP_LOGE(TAG, "Failed to initialize crypto with new passphrase");
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "{\"action\":\"set_passphrase\",\"status\":\"error\",\"message\":\"crypto_init_failed\"}");
        send_response(buf);
        return;
    }

    /* Trigger reconnection if disconnected */
    if (ble_state_ == BleState::Disconnected || ble_state_ == BleState::Idle) {
        ble_state_ = BleState::Disconnected;
        reconnect_at_ms_ = esphome::millis();  /* Reconnect immediately */
        ESP_LOGI(TAG, "Triggering BLE reconnection after passphrase set");
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "{\"action\":\"set_passphrase\",\"status\":\"ok\"}");
    send_response(buf);
}

void AvionMeshHub::handle_generate_passphrase() {
    db_.generate_passphrase();
    const std::string &passphrase = db_.passphrase();

    ESP_LOGI(TAG, "Generated passphrase: %s", passphrase.c_str());

    /* Reinitialize crypto with new passphrase */
    crypto_initialized_ = false;
    mesh_initialized_ = false;

    if (!init_crypto()) {
        ESP_LOGE(TAG, "Failed to initialize crypto with generated passphrase");
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "{\"action\":\"generate_passphrase\",\"status\":\"error\",\"message\":\"crypto_init_failed\"}");
        send_response(buf);
        return;
    }

    /* Trigger reconnection if disconnected */
    if (ble_state_ == BleState::Disconnected || ble_state_ == BleState::Idle) {
        ble_state_ = BleState::Disconnected;
        reconnect_at_ms_ = esphome::millis();  /* Reconnect immediately */
        ESP_LOGI(TAG, "Triggering BLE reconnection after passphrase generated");
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "{\"action\":\"generate_passphrase\",\"status\":\"ok\",\"passphrase\":\"%s\"}",
             passphrase.c_str());
    send_response(buf);
}

void AvionMeshHub::handle_factory_reset() {
    ESP_LOGI(TAG, "Factory reset: clearing all devices, groups, and passphrase");

    /* Remove all MQTT discovery configs */
    for (auto &dev : db_.devices()) {
        discovery_.remove_light(dev.avion_id);
    }
    for (auto &grp : db_.groups()) {
        discovery_.remove_light(grp.group_id);
    }

    /* Clear mesh context by reinitializing */
    std::memset(&mesh_ctx_, 0, sizeof(mesh_ctx_));
    mesh_initialized_ = false;
    crypto_initialized_ = false;

    /* Clear database */
    db_.clear();

    /* Reload (empty) */
    db_.load();

    /* Reinitialize mesh if passphrase is set */
    if (!db_.passphrase().empty()) {
        init_crypto();
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "{\"action\":\"factory_reset\",\"status\":\"ok\"}");
    send_response(buf);
}

/* ---- Light commands from MQTT (separate topics, bare payloads) ---- */

void AvionMeshHub::on_switch_command(uint16_t avion_id, const std::string &payload) {
    uint8_t brightness = (payload == "ON") ? 255 : 0;
    Command cmd;
    cmd_brightness(avion_id, brightness, cmd);
    send_cmd(mesh_ctx_, cmd);
    auto &state = device_states_[avion_id];
    state.brightness = brightness;
    state.brightness_known = true;
    publish_device_state(avion_id);
}

void AvionMeshHub::on_brightness_command(uint16_t avion_id, const std::string &payload) {
    uint8_t brightness = static_cast<uint8_t>(strtoul(payload.c_str(), nullptr, 10));

    uint32_t now = esphome::millis();
    auto it = last_brightness_ms_.find(avion_id);
    if (it != last_brightness_ms_.end() &&
        (now - it->second) < RAPID_DIM_THRESHOLD_MS) {
        it->second = now;
        auto &state = device_states_[avion_id];
        state.brightness = brightness;
        state.brightness_known = true;
        publish_device_state(avion_id);
        return;
    }
    last_brightness_ms_[avion_id] = now;

    Command cmd;
    cmd_brightness(avion_id, brightness, cmd);
    send_cmd(mesh_ctx_, cmd);
    auto &state = device_states_[avion_id];
    state.brightness = brightness;
    state.brightness_known = true;
    publish_device_state(avion_id);
}

void AvionMeshHub::on_color_temp_command(uint16_t avion_id, const std::string &payload) {
    uint16_t mireds = static_cast<uint16_t>(strtoul(payload.c_str(), nullptr, 10));
    uint16_t kelvin = mireds > 0 ? 1000000u / mireds : 3000;
    Command cmd;
    cmd_color_temp(avion_id, kelvin, cmd);
    send_cmd(mesh_ctx_, cmd);
    auto &state = device_states_[avion_id];
    state.color_temp = kelvin;
    state.color_temp_known = true;
    publish_device_state(avion_id);
}

/* ---- Helpers ---- */

void AvionMeshHub::publish_all_discovery() {
    for (auto &dev : db_.devices()) {
        bool has_dim = has_dimming(dev.product_type);
        bool has_ct = has_color_temp(dev.product_type);
        discovery_.publish_light(dev.avion_id, dev.name, has_dim, has_ct,
                                  product_name(dev.product_type));
    }
    for (auto &grp : db_.groups()) {
        discovery_.publish_light(grp.group_id, grp.name, true, true);
    }
}

void AvionMeshHub::subscribe_all_commands() {
    auto *mqtt = esphome::mqtt::global_mqtt_client;
    if (!mqtt)
        return;

    auto subscribe_light = [this, mqtt](uint16_t id, bool has_brightness, bool has_ct) {
        mqtt->subscribe(discovery_.command_topic(id),
                        [this, id](const std::string &, const std::string &payload) {
                            on_switch_command(id, payload);
                        }, 0);
        if (has_brightness) {
            mqtt->subscribe(discovery_.brightness_command_topic(id),
                            [this, id](const std::string &, const std::string &payload) {
                                on_brightness_command(id, payload);
                            }, 0);
        }
        if (has_ct) {
            mqtt->subscribe(discovery_.color_temp_command_topic(id),
                            [this, id](const std::string &, const std::string &payload) {
                                on_color_temp_command(id, payload);
                            }, 0);
        }
    };

    for (auto &dev : db_.devices()) {
        subscribe_light(dev.avion_id, has_dimming(dev.product_type),
                        has_color_temp(dev.product_type));
    }
    for (auto &grp : db_.groups()) {
        subscribe_light(grp.group_id, true, true);
    }

    mqtt_subscribed_ = true;
    ESP_LOGI(TAG, "MQTT subscriptions active");
}

void AvionMeshHub::send_response(const std::string &payload) {
    auto *mqtt = esphome::mqtt::global_mqtt_client;
    if (mqtt)
        mqtt->publish(discovery_.management_response_topic(), payload, 0, false);
}

void AvionMeshHub::sync_time() {
    time_t now;
    time(&now);
    struct tm *t = localtime(&now);
    if (!t || t->tm_year < 120)
        return;

    ESP_LOGI(TAG, "Syncing mesh time: %04d-%02d-%02d %02d:%02d:%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);

    Command cmd;
    cmd_set_date(static_cast<uint16_t>(t->tm_year + 1900),
                 static_cast<uint8_t>(t->tm_mon + 1),
                 static_cast<uint8_t>(t->tm_mday), cmd);
    send_cmd(mesh_ctx_, cmd);

    cmd_set_time(static_cast<uint8_t>(t->tm_hour),
                 static_cast<uint8_t>(t->tm_min),
                 static_cast<uint8_t>(t->tm_sec), cmd);
    send_cmd(mesh_ctx_, cmd);
}

void AvionMeshHub::read_all_dimming() {
    ESP_LOGI(TAG, "Broadcasting READ DIMMING");
    Command cmd;
    cmd_read_all_dimming(cmd);
    send_cmd(mesh_ctx_, cmd);
}

void AvionMeshHub::read_all_color() {
    ESP_LOGI(TAG, "Broadcasting READ COLOR");
    Command cmd;
    cmd_read_all_color(cmd);
    send_cmd(mesh_ctx_, cmd);
}

void AvionMeshHub::publish_device_state(uint16_t avion_id) {
    auto it = device_states_.find(avion_id);
    if (it == device_states_.end())
        return;

    auto &state = it->second;
    if (!state.brightness_known)
        return;

    auto *dev = db_.find_device(avion_id);
    if (!dev)
        return;

    discovery_.publish_on_off_state(avion_id, state.brightness > 0);
    discovery_.publish_brightness_state(avion_id, state.brightness);

    if (state.color_temp_known && has_color_temp(dev->product_type)) {
        discovery_.publish_color_temp_state(avion_id, state.color_temp);
    }

    if (web_handler_) {
        char buf[128];
        int len;
        if (state.color_temp_known) {
            len = snprintf(buf, sizeof(buf),
                           "{\"avion_id\":%u,\"brightness\":%u,\"color_temp\":%u}",
                           avion_id, state.brightness, state.color_temp);
        } else {
            len = snprintf(buf, sizeof(buf),
                           "{\"avion_id\":%u,\"brightness\":%u}",
                           avion_id, state.brightness);
        }
        web_handler_->send_event("state", std::string(buf, len));
    }
}

}  // namespace avionmesh
