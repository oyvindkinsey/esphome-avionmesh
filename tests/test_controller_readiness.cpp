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
