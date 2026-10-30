#pragma once

#include "device_db.h"
#include "mqtt_discovery.h"
#include "mesh_capture.h"
#include "controller_events.h"
#include "controller_tables.h"
#include "keypad_setup_journal.h"
#include "absolute_dimmer.h"
#include "local_dimmer.h"
#include "controller_profile.h"

#include "esphome/core/component.h"
#include "esphome/components/esp32_ble/ble.h"
#include "esphome/components/esp32_ble/ble_scan_result.h"

#include <recsrmesh/csrmesh.h>
#include <avionmesh/avionmesh.h>

#include <functional>
#include <map>
#include <mutex>
#include <set>
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
        SetMqttExposed,
        SetLocalDimmerMode,
        SaveDb,
        RenameDevice,
        RenameGroup,
        RefreshControllerBindings,
        SetupControllerButtons,
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

// Vendor/product codes from a device UUID announce. Byte layout matches the
// native app's decoder: uuid[6] is the vendor code, uuid[8..9] (big-endian)
// is the product code — verified against the cloud product catalog
// (e.g. vendor 20 + product 18 = MicroEdge, catalog id 162).
void decode_uuid_identity(const uint8_t *uuid, size_t uuid_len, uint8_t &vendor_code,
                          uint8_t &product_code);

struct DiscoveredDevice {
    uint16_t device_id;
    uint8_t fw_major{0}, fw_minor{0}, fw_patch{0};
    uint8_t flags{0};
    uint16_t vendor_id{0};
    uint8_t csr_product_id{0};
    // True when the ping reply was too short to carry fw/vendor/product
    // fields (some devices answer with a truncated payload).
    bool short_reply{false};
};

enum class BleState : uint8_t {
    Idle,
    Scanning,
    Connecting,
    Discovering,
    Ready,
    Disconnected,
};

class AvionMeshHub : public esphome::Component {
    friend class AvionMeshWebHandler;

 public:
    void set_passphrase(const std::string &passphrase) { passphrase_ = passphrase; }
    void set_single_device(bool single_device) { single_device_ = single_device; }

    void setup() override;
    void loop() override;
    void dump_config() override;
    float get_setup_priority() const override;

    /* esp32_ble event handlers (callback-based since ESPHome 2026.4.0) */
    void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
    void gap_scan_event_handler(const esphome::esp32_ble::BLEScanResult &result);
    void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                             esp_ble_gattc_cb_param_t *param);

 protected:
    std::string passphrase_;  // From YAML (if provided), used to initialize db
    bool single_device_{false};

    csrmesh::MeshContext mesh_ctx_{};
    bool mesh_initialized_{false};    // true when crypto is ready AND BLE is connected
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
    ControllerBindings controller_bindings_;
    KeypadSetupJournal keypad_setup_journal_;
    enum class KeypadSetupPhase : uint8_t {
        WaitWake,
        Baseline,
        WaitBaseline,
        Plan,
        SlotRead,
        WaitSlotRead,
        Program,
        WaitProgram,
        Verify,
        WaitVerify,
        Finish
    };
    bool keypad_setup_active_{false}, keypad_setup_internal_{false};
    bool keypad_setup_mapping_done_{false}, keypad_setup_mapping_ok_{false};
    bool keypad_setup_slot_done_{false}, keypad_setup_slot_ok_{false};
    bool keypad_setup_program_done_{false}, keypad_setup_reconcile_{false};
    uint16_t keypad_setup_target_{0};
    uint8_t keypad_setup_slot_{0};
    uint32_t keypad_setup_request_{0xb0000000u};
    uint32_t keypad_setup_wait_since_ms_{0};
    uint64_t keypad_setup_mesh_tag_{0};
    KeypadSetupPhase keypad_setup_phase_{KeypadSetupPhase::Baseline};
    KeypadSetupPlan keypad_setup_plan_;
    ControllerSlotRecords keypad_setup_working_{};
    std::array<uint8_t, 8> keypad_setup_slot_reply_{};
    std::set<uint16_t> keypad_setup_attempted_;
    std::map<uint16_t, std::string> keypad_setup_status_;
    void start_keypad_setup(uint16_t target, bool manual);
    void process_keypad_setup();
    void finish_keypad_setup(const std::string &status);
    void keypad_setup_mapping_finished(uint16_t target, const char *status);
    void keypad_setup_read_reply(const char *status, const uint8_t *payload, size_t length);
    void keypad_setup_program_reply(const char *status);
    std::string keypad_setup_status(uint16_t target) const;
    void keypad_setup_emit();
    MqttDiscovery discovery_;

    bool mgmt_subscribed_{false};
    bool mqtt_subscribed_{false};
    bool mesh_mqtt_exposed_{false};
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
    struct ScanCandidate {
        uint32_t uuid_hash{0};
        uint8_t vendor_code{0};
        uint8_t product_code{0};
    };
    std::vector<ScanCandidate> scan_candidates_;

    /* Per-device cached state for complete MQTT publishes */
    std::map<uint16_t, DeviceState> device_states_;

    /* Rapid dimming detection */
    static constexpr uint32_t RAPID_DIM_THRESHOLD_MS = 750;
    std::map<uint16_t, uint32_t> last_brightness_ms_;

    static constexpr uint32_t STATE_REFRESH_INTERVAL_MS = 60000;

    uint32_t rx_count_{0};
    MeshCapture capture_;
    bool capture_reporting_{false};
    void process_capture();
    void report_capture_status(const char *action);
    ControllerPacketQueue controller_packets_;
    ControllerDuplicateWindow controller_duplicates_;
    std::map<uint16_t, AbsoluteDimmerDirection> controller_dimmer_levels_;
    std::map<uint16_t, LocalDimmerReports> controller_local_levels_;
    uint32_t local_baseline_probe_ms_{0};
    uint8_t local_baseline_probe_attempts_{0};
    bool zero_dimming_query_seen_{false};
    uint32_t zero_dimming_query_ms_{0};
    uint32_t controller_events_published_{0}, controller_events_duplicates_{0};
    uint32_t controller_events_expired_{0}, controller_events_offline_{0};
    uint32_t controller_events_failed_{0};
    bool controller_mqtt_was_connected_{false};
    bool controller_availability_known_{false}, controller_available_{false};
    uint32_t controller_mqtt_since_ms_{0}, controller_mesh_since_ms_{0};
    void process_controller_events();
    void set_local_dimmer_mode(uint16_t target, bool enabled);
    std::string controller_input_status(uint16_t target) const;
    std::string controller_bindings_json(uint16_t target);
    void publish_controller_actions(uint16_t target);
    void start_controller_binding_refresh(uint16_t target);
    void process_controller_binding_refresh();
    void binding_refresh_reply(const char *status, const uint8_t *payload, size_t length);
    void finish_controller_binding_refresh(const char *status);
    bool binding_refresh_active_{false}, binding_refresh_waiting_{false};
    void handle_battery_keypad_wake(uint16_t source, uint8_t opcode, const uint8_t *payload,
                                    size_t length, uint32_t received);
    uint32_t controller_read_timeout_ms() const;
    bool binding_refresh_waiting_wake_{false};
    uint8_t binding_refresh_product_{0};
    uint32_t binding_refresh_wait_since_ms_{0};
    uint16_t binding_refresh_target_{0};
    uint8_t binding_refresh_round_{0}, binding_refresh_slot_{0}, binding_refresh_attempt_{0};
    uint32_t binding_refresh_request_{0x80000000u};
    uint64_t binding_refresh_mesh_tag_{0};
    std::array<ControllerSlotRecords, 2> binding_refresh_records_{};
    std::array<std::array<uint8_t, 5>, 2> binding_refresh_counts_{};
    std::map<uint16_t, std::string> binding_refresh_status_;
    bool controller_read_active_{false};
    bool controller_read_probe_{false}, controller_probe_count_{false};
    bool controller_probe_padded_{false};
    uint8_t controller_probe_noun_{0};
    uint8_t controller_probe_transaction_{0};
    uint16_t controller_read_target_{0};
    uint8_t controller_read_slot_{0};
    uint32_t controller_read_id_{0}, controller_read_since_ms_{0};
    void report_controller_read(const char *status, const uint8_t *payload = nullptr,
                                size_t length = 0);
    struct ControllerSlotRead {
        std::string raw;
        uint32_t received_ms{0};
    };
    std::map<uint16_t, std::array<ControllerSlotRead, 4>> controller_slot_reads_;
    bool controller_program_active_{false}, controller_program_clear_{false};
    bool controller_program_secondary_{false};
    uint16_t controller_program_target_{0};
    uint8_t controller_program_slot_{0}, controller_program_transaction_{0};
    uint32_t controller_program_id_{0}, controller_program_since_ms_{0};
    void report_controller_program(const char *status, const uint8_t *payload = nullptr,
                                   size_t length = 0);

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
    void on_mesh_rx(uint16_t mcp_source, uint16_t crypto_source, uint8_t opcode,
                    const uint8_t *payload, size_t payload_len);

    /* MQTT */
    void on_mqtt_command(const std::string &payload);
    void handle_scan_unassociated();
    void handle_claim_device(uint32_t uuid_hash, uint16_t device_id, const std::string &name,
                             uint8_t product_type);
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
    bool save_mesh_mqtt_exposure();
    void subscribe_all_commands();
    void send_response(const std::string &payload);
    void report_claim_result(bool success, uint16_t device_id, const char *message = nullptr);
    void sync_time();
    void read_all_dimming();
    void read_all_color();
    void publish_device_state(uint16_t avion_id);
    void check_group_state_latch(uint16_t avid);

    /* Virtual seams — overridden by TestHub in tests; default impls use real globals */
    virtual void do_mesh_send(const Command &cmd);
    virtual void do_mqtt_publish(const std::string &topic, const std::string &payload, bool retain);
    virtual void do_mqtt_subscribe(
        const std::string &topic, std::function<void(const std::string &, const std::string &)> cb);
    virtual void do_sse_emit(const std::string &event, const std::string &data);
};

}  // namespace avionmesh
