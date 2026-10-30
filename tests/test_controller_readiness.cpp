#include "mock_hub.h"
#include <ArduinoJson.h>
#include <gtest/gtest.h>

using namespace avionmesh;
namespace {
class ReadinessHub : public TestHub {
 public:
    using AvionMeshHub::controller_input_status;
    using AvionMeshHub::report_claim_result;
    using AvionMeshHub::set_local_dimmer_mode;
    void baseline(uint16_t id) { controller_local_levels_[id].observe(1, 0, 1000, true); }
    void connection(bool mqtt_connected, bool mesh_ready) {
        mesh_initialized_ = mesh_ready;
        update_controller_input_readiness(mqtt_connected);
    }
    AbsoluteDimmerDirection::Change report(uint16_t id, uint32_t sequence, uint8_t level,
                                           uint32_t received_ms, bool query = false) {
        return observe_controller_local_report(id, sequence, level, received_ms, query);
    }
};
}  // namespace

TEST(ControllerReadiness, ClaimResultUsesPersistedModeAndDoesNotPromiseAssignment) {
    ReadinessHub hub;
    ASSERT_TRUE(hub.db().add_device(33001, 91, "Fresh", true));
    ASSERT_TRUE(hub.db().add_device(33002, 91, "Imported native"));
    ASSERT_TRUE(hub.db().add_device(33003, 81, "Office slider"));
    for (uint16_t id : {33001, 33002, 33003}) {
        hub.report_claim_result(true, id);
        ASSERT_FALSE(hub.sse_events.empty());
        JsonDocument event;
        ASSERT_FALSE(deserializeJson(event, hub.sse_events.back().second));
        EXPECT_EQ(hub.sse_events.back().first, "claim_result");
        EXPECT_EQ(event["status"].as<std::string>(), "ok");
        EXPECT_TRUE(event["mqtt_exposed"].as<bool>());
        EXPECT_EQ(event["ha_input_status"].as<std::string>(),
                  id == 33001 ? "waiting_for_baseline" : "native_commands");
        EXPECT_EQ(event["ha_local_level"].as<bool>(), id == 33001);
        EXPECT_EQ(event["ha_native_assignment_status"].as<std::string>(), "unknown");
        EXPECT_FALSE(event["ha_native_assignment_programming"].as<bool>());
    }
    hub.report_claim_result(false, 33001, "timeout");
    JsonDocument failed;
    ASSERT_FALSE(deserializeJson(failed, hub.sse_events.back().second));
    EXPECT_TRUE(failed["ha_input_status"].isNull());
    EXPECT_TRUE(hub.mesh_sends.empty());
}

TEST(ControllerReadiness, DecoderSelectionResetsBaselineAndPreservesManagedTargets) {
    ReadinessHub hub;
    ASSERT_TRUE(hub.db().add_device(33001, 91, "Accessory", true));
    ASSERT_TRUE(hub.db().add_device(33003, 81, "Office slider"));
    ASSERT_TRUE(hub.db().add_group(757, "HA office slider"));
    hub.db().find_group(757)->ha_event_target = true;
    hub.test_setup();
    hub.baseline(33001);
    EXPECT_EQ(hub.controller_input_status(33001), "ready_local_reports");
    hub.set_local_dimmer_mode(33001, false);
    EXPECT_EQ(hub.controller_input_status(33001), "native_commands");
    hub.set_local_dimmer_mode(33001, true);
    EXPECT_EQ(hub.controller_input_status(33001), "waiting_for_baseline");
    JsonDocument result;
    ASSERT_FALSE(deserializeJson(result, hub.sse_events.back().second));
    EXPECT_EQ(result["ha_input_status"].as<std::string>(), "waiting_for_baseline");
    hub.set_local_dimmer_mode(33003, true);
    EXPECT_FALSE(hub.db().find_device(33003)->ha_local_level);
    EXPECT_EQ(hub.controller_input_status(33003), "native_commands");
    ASSERT_EQ(hub.db().groups().size(), 1u);
    EXPECT_TRUE(hub.db().find_group(757)->ha_event_target);
    EXPECT_TRUE(hub.db().find_group(757)->member_ids.empty());
    EXPECT_TRUE(hub.mesh_sends.empty());  // No native programming or writes.
}

namespace {
std::map<std::string, std::string> retained_configs(const ReadinessHub &hub) {
    std::map<std::string, std::string> result;
    for (const auto &[topic, payload, retained] : hub.mqtt_publishes)
        if (retained && topic.rfind("homeassistant/", 0) == 0)
            result[topic] = payload;
    return result;
}
std::string input_availability(const ReadinessHub &hub) {
    std::string last;
    for (const auto &[topic, payload, retained] : hub.mqtt_publishes)
        if (topic == "avionmesh/controller/33001/input/availability" && retained)
            last = payload;
    return last;
}
void expect_single_waiting_status(const ReadinessHub &hub) {
    ASSERT_EQ(hub.sse_events.size(), 1u);
    EXPECT_EQ(hub.sse_events[0].first, "controller_input_status");
    JsonDocument event;
    ASSERT_FALSE(deserializeJson(event, hub.sse_events[0].second));
    EXPECT_EQ(event["controller_id"], 33001);
    EXPECT_EQ(event["status"], "waiting_for_baseline");
}
}

TEST(ControllerReadiness, BleLossRevokesLocalInputUntilFreshBaselineWithoutRepeatedPublishes) {
    ReadinessHub hub;
    ASSERT_TRUE(hub.db().add_device(33001, 91, "Local accessory", true));
    ASSERT_TRUE(hub.db().add_device(33002, 91, "Native accessory"));
    ASSERT_TRUE(hub.db().add_device(33003, 81, "Office slider"));
    hub.test_setup();
    hub.connection(true, true);
    EXPECT_FALSE(hub.report(33001, 1, 100, 1000, true).accepted);
    EXPECT_EQ(hub.controller_input_status(33001), "ready_local_reports");
    EXPECT_EQ(input_availability(hub), "online");
    const auto configs = retained_configs(hub);
    ASSERT_FALSE(configs.empty());
    hub.clear_captures();

    hub.connection(true, false);
    EXPECT_EQ(hub.controller_input_status(33001), "waiting_for_baseline");
    EXPECT_EQ(hub.controller_input_status(33002), "native_commands");
    EXPECT_EQ(hub.controller_input_status(33003), "native_commands");
    EXPECT_EQ(input_availability(hub), "offline");
    expect_single_waiting_status(hub);
    ASSERT_EQ(hub.mqtt_publishes.size(), 1u);
    for (unsigned loop = 0; loop < 100; ++loop)
        hub.connection(true, false);
    EXPECT_EQ(hub.mqtt_publishes.size(), 1u);
    EXPECT_EQ(hub.sse_events.size(), 1u);
    EXPECT_FALSE(hub.report(33001, 2, 255, 2000).accepted);
    EXPECT_EQ(input_availability(hub), "offline");

    hub.connection(true, true);
    EXPECT_EQ(hub.controller_input_status(33001), "waiting_for_baseline");
    EXPECT_EQ(input_availability(hub), "offline");
    EXPECT_EQ(hub.mqtt_publishes.size(), 1u); // BLE recovery cannot restore a stale baseline.
    EXPECT_FALSE(hub.report(33001, 3, 255, 3000, true).accepted);
    EXPECT_EQ(hub.controller_input_status(33001), "ready_local_reports");
    EXPECT_EQ(input_availability(hub), "online");
    const auto refreshed = retained_configs(hub);
    ASSERT_EQ(refreshed.size(), 10u);
    for (const auto &[topic, payload] : refreshed) {
        ASSERT_TRUE(configs.contains(topic));
        EXPECT_FALSE(payload.empty());
        EXPECT_EQ(payload, configs.at(topic)); // Same event/trigger ID and complete config.
    }
    EXPECT_TRUE(hub.report(33001, 4, 255, 3100).accepted);
    EXPECT_TRUE(hub.mesh_sends.empty());
}

TEST(ControllerReadiness, MqttReconnectPublishesBaselineOfflineBeforeSharedRecovery) {
    ReadinessHub hub;
    ASSERT_TRUE(hub.db().add_device(33001, 91, "Local accessory", true));
    ASSERT_TRUE(hub.db().add_device(33002, 91, "Native accessory"));
    ASSERT_TRUE(hub.db().add_device(33003, 81, "Office slider"));
    hub.test_setup();
    hub.connection(true, true);
    hub.report(33001, 1, 100, 1000, true);
    const auto configs = retained_configs(hub);
    hub.clear_captures();

    hub.connection(false, true);
    EXPECT_TRUE(hub.mqtt_publishes.empty()); // No attempts through a disconnected MQTT client.
    EXPECT_EQ(hub.controller_input_status(33001), "waiting_for_baseline");
    expect_single_waiting_status(hub);
    for (unsigned loop = 0; loop < 100; ++loop)
        hub.connection(false, true);
    EXPECT_TRUE(hub.mqtt_publishes.empty());
    EXPECT_EQ(hub.sse_events.size(), 1u);

    hub.connection(true, true); // Actual reconnect discovery path in the hub helper.
    EXPECT_EQ(input_availability(hub), "offline");
    EXPECT_EQ(hub.controller_input_status(33001), "waiting_for_baseline");
    EXPECT_EQ(hub.controller_input_status(33002), "native_commands");
    EXPECT_EQ(hub.controller_input_status(33003), "native_commands");
    EXPECT_EQ(retained_configs(hub), configs);
    const auto count = hub.mqtt_publishes.size();
    hub.connection(true, true);
    EXPECT_EQ(hub.mqtt_publishes.size(), count);
    EXPECT_FALSE(hub.report(33003, 2, 255, 2000).accepted); // Other models cannot seed local input.
    EXPECT_FALSE(hub.report(33002, 2, 255, 2000).accepted); // Native mode cannot seed local input.
    EXPECT_EQ(input_availability(hub), "offline");
    EXPECT_FALSE(hub.report(33001, 2, 255, 2000).accepted); // First local report is a silent seed.
    EXPECT_EQ(input_availability(hub), "online");
    EXPECT_TRUE(hub.report(33001, 3, 255, 2100).accepted);
    EXPECT_TRUE(hub.mesh_sends.empty());
}
