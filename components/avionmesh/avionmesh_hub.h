#pragma once

#include "device_db.h"
#include "mqtt_discovery.h"

#include "esphome/core/component.h"
#include "esphome/components/esp32_ble/ble.h"
#include "esphome/components/esp32_ble/ble_scan_result.h"

#include <recsrmesh/csrmesh.h>
#include <avionmesh/avionmesh.h>

#include <map>
#include <mutex>
#include <vector>

namespace avionmesh {

struct DeferredAction {
    enum Type : uint8_t {
        Control,
        AddDiscovered,
        UnclaimDevice,
        CreateGroup,
        DeleteGroup,
        AddToGroup,
        RemoveFromGroup,
        Import,
    };
    Type type;
    uint16_t id1{0};
    uint16_t id2{0};
    uint8_t product_type{0};
    int brightness{-1};
    int color_temp{-1};
    std::string name;
    std::string body;  // for Import
};

class AvionMeshWebHandler;

struct DeviceState {
    uint8_t brightness{0};
    uint16_t color_temp{0};
    bool brightness_known{false};
    bool color_temp_known{false};
};

struct DiscoveredDevice {
    uint16_t device_id;
    uint8_t fw_major, fw_minor, fw_patch;
    uint8_t flags;
    uint16_t vendor_id;
    uint8_t csr_product_id;
};

enum class BleState : uint8_t {
    Idle,
    Scanning,
    Connecting,
    Discovering,
    Ready,
    Disconnected,
};

class AvionMeshHub : public esphome::Component,
                     public esphome::esp32_ble::GAPEventHandler,
                     public esphome::esp32_ble::GAPScanEventHandler,
                     public esphome::esp32_ble::GATTcEventHandler {
    friend class AvionMeshWebHandler;

 public:
    void set_passphrase(const std::string &passphrase) { passphrase_ = passphrase; }

    void setup() override;
    void loop() override;
    void dump_config() override;
    float get_setup_priority() const override;

    /* esp32_ble event handler interfaces */
    void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) override;
    void gap_scan_event_handler(const esphome::esp32_ble::BLEScanResult &result) override;
    void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                              esp_ble_gattc_cb_param_t *param) override;

 protected:
    std::string passphrase_;  // From YAML (if provided), used to initialize db

    csrmesh::MeshContext mesh_ctx_{};
    bool mesh_initialized_{false};  // true when crypto is ready AND BLE is connected
    bool crypto_initialized_{false};  // true when csrmesh::init() succeeded

    /* BLE connection management */
    BleState ble_state_{BleState::Idle};
    esp_bd_addr_t bridge_bda_{};
    int best_rssi_{-999};
    uint32_t scan_start_ms_{0};
    static constexpr uint32_t SCAN_WINDOW_MS = 5000;
    static constexpr uint32_t RECONNECT_DELAY_MS = 3000;
    uint32_t reconnect_at_ms_{0};

    esp_gatt_if_t gattc_if_{0};
    uint16_t conn_id_{0};
    uint16_t char_low_handle_{0};
    uint16_t char_high_handle_{0};
    bool gattc_registered_{false};
    uint16_t app_id_{0};

    /* Association state */
    csrmesh::protocol::Context proto_ctx_{};
    bool associating_{false};
    uint32_t association_start_ms_{0};
    static constexpr uint32_t ASSOCIATION_TIMEOUT_MS = 30000;

    DeviceDB db_;
    MqttDiscovery discovery_;

    bool mgmt_subscribed_{false};
    bool mqtt_subscribed_{false};
    bool initial_read_done_{false};
    bool time_synced_{false};

    /* Mesh discovery state */
    bool discovering_mesh_{false};
    std::vector<DiscoveredDevice> discovered_devices_;

    /* Examine device state */
    bool examining_{false};
    uint16_t examine_target_{0};

    /* Web handler */
    AvionMeshWebHandler *web_handler_{nullptr};
    bool web_registered_{false};

    /* ID ranges for auto-assignment */
    static constexpr uint16_t MIN_DEVICE_ID = 32896;
    static constexpr uint16_t MAX_DEVICE_ID = 65407;
    static constexpr uint16_t MIN_GROUP_ID = 256;
    static constexpr uint16_t MAX_GROUP_ID = 24575;

    /* Deferred web requests (set by HTTP thread, consumed by main loop) */
    volatile bool pending_discover_mesh_{false};
    volatile bool pending_scan_unassoc_{false};
    volatile bool pending_examine_{false};
    volatile uint16_t pending_examine_id_{0};
    volatile bool pending_claim_auto_{false};
    uint32_t pending_claim_uuid_hash_{0};
    std::string pending_claim_name_;
    uint8_t pending_claim_product_type_{0};

    /* Thread-safe action queue (httpd thread → main loop) */
    std::mutex action_mutex_;
    std::vector<DeferredAction> pending_actions_;
    void process_deferred_actions();

    /* Unassociated scan state */
    bool scanning_unassociated_{false};
    std::vector<uint32_t> scan_uuid_hashes_;

    /* Per-device cached state for complete MQTT publishes */
    std::map<uint16_t, DeviceState> device_states_;

    /* Rapid dimming detection */
    static constexpr uint32_t RAPID_DIM_THRESHOLD_MS = 750;
    std::map<uint16_t, uint32_t> last_brightness_ms_;

    static constexpr uint32_t STATE_REFRESH_INTERVAL_MS = 60000;

    uint32_t rx_count_{0};

    /* GAP scanning */
    void start_scan();
    void stop_scan_and_connect();

    /* GATTC connection */
    void connect_to_best();
    void on_connected(esp_gatt_if_t gattc_if, uint16_t conn_id);
    void on_service_discovery_complete();
    void on_disconnected();

    /* CSRMesh BLE write */
    int ble_write(csrmesh::Characteristic ch, const uint8_t *data, size_t len, bool response);

    /* Mesh RX handler */
    void on_mesh_rx(uint16_t mcp_source, uint16_t crypto_source,
                    uint8_t opcode, const uint8_t *payload, size_t payload_len);

    /* MQTT */
    void on_mqtt_command(const std::string &payload);
    void handle_scan_unassociated();
    void handle_claim_device(uint32_t uuid_hash, uint16_t device_id,
                              const std::string &name, uint8_t product_type);
    void handle_claim_device_auto();
    uint16_t next_device_id();
    uint16_t next_group_id();
    void handle_unclaim_device(uint16_t avion_id);
    void handle_create_group(uint16_t group_id, const std::string &name);
    void handle_delete_group(uint16_t group_id);
    void handle_add_to_group(uint16_t avion_id, uint16_t group_id);
    void handle_remove_from_group(uint16_t avion_id, uint16_t group_id);
    void handle_discover_mesh();
    void handle_add_discovered(uint16_t device_id, const std::string &name, uint8_t product_type);
    void handle_examine_device(uint16_t avion_id);
    void handle_set_passphrase(const std::string &passphrase);
    void handle_generate_passphrase();
    void handle_factory_reset();
    void on_switch_command(uint16_t avion_id, const std::string &payload);
    void on_brightness_command(uint16_t avion_id, const std::string &payload);
    void on_color_temp_command(uint16_t avion_id, const std::string &payload);

    /* Crypto initialization - returns true if successful */
    bool init_crypto();
    void update_mesh_initialized();

    void publish_all_discovery();
    void subscribe_all_commands();
    void send_response(const std::string &payload);
    void sync_time();
    void read_all_dimming();
    void read_all_color();
    void publish_device_state(uint16_t avion_id);
};

}  // namespace avionmesh
