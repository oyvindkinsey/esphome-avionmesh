// Tests: HTTP deferred actions (DeferredAction queue) → BLE mesh + MQTT echo.
// Exercises the path that the web handler pushes into pending_actions_.

#include "mock_hub.h"
#include <gtest/gtest.h>

using namespace avionmesh;

static constexpr uint16_t DEV = 32900;

class ApiControlTest : public ::testing::Test {
protected:
    TestHub hub;

    void SetUp() override {
        hub.db().add_device(DEV, 90, "Test Light");
        hub.db().find_device(DEV)->mqtt_exposed = true;
        hub.test_setup();
    }
};

// Control: brightness only
TEST_F(ApiControlTest, ControlBrightness_SendsToBle) {
    DeferredAction act;
    act.type       = DeferredAction::Control;
    act.id1        = DEV;
    act.brightness = 180;
    act.color_temp = -1;
    hub.push_action(act);

    ASSERT_EQ(hub.mesh_sends.size(), 1u);
    auto &cmd = hub.mesh_sends[0];
    EXPECT_EQ(cmd.dest_id, DEV);
    EXPECT_EQ(cmd.payload[1], 0x0A);   // Noun::Dimming
    EXPECT_EQ(cmd.payload[5], 180);
}

TEST_F(ApiControlTest, ControlBrightness_EchoesStateToMqtt) {
    DeferredAction act;
    act.type       = DeferredAction::Control;
    act.id1        = DEV;
    act.brightness = 180;
    act.color_temp = -1;
    hub.push_action(act);

    bool found = false;
    for (auto &[topic, payload, retain] : hub.mqtt_publishes)
        if (topic.find("/state") != std::string::npos &&
            payload.find("\"brightness\":180") != std::string::npos)
            found = true;
    EXPECT_TRUE(found);
}

// Control: color_temp only
TEST_F(ApiControlTest, ControlColorTemp_SendsCorrectKelvin) {
    DeferredAction act;
    act.type       = DeferredAction::Control;
    act.id1        = DEV;
    act.brightness = -1;
    act.color_temp = 3000;
    hub.push_action(act);

    ASSERT_EQ(hub.mesh_sends.size(), 1u);
    auto &cmd = hub.mesh_sends[0];
    EXPECT_EQ(cmd.payload[1], 0x1D);  // Noun::Color
    uint16_t kelvin = (static_cast<uint16_t>(cmd.payload[6]) << 8) | cmd.payload[7];
    EXPECT_EQ(kelvin, 3000u);
}

// Control: both brightness and color_temp → two mesh sends
TEST_F(ApiControlTest, ControlBrightnessAndColorTemp_TwoMeshSends) {
    DeferredAction act;
    act.type       = DeferredAction::Control;
    act.id1        = DEV;
    act.brightness = 200;
    act.color_temp = 4000;
    hub.push_action(act);

    EXPECT_EQ(hub.mesh_sends.size(), 2u);
}

// SetMqttExposed: device becomes exposed → MQTT discovery published + topics subscribed
TEST_F(ApiControlTest, SetMqttExposed_True_PublishesDiscoveryAndSubscribes) {
    // Add a second unexposed device
    static constexpr uint16_t DEV2 = 32901;
    hub.db().add_device(DEV2, 90, "New Light");
    hub.test_setup();
    hub.clear_captures();

    DeferredAction act;
    act.type = DeferredAction::SetMqttExposed;
    act.id1  = DEV2;
    act.id2  = 1;  // exposed = true
    hub.push_action(act);

    // Discovery topic must have been published (retained)
    bool discovery_pub = false;
    for (auto &[topic, payload, retain] : hub.mqtt_publishes)
        if (topic.find("homeassistant/light/") != std::string::npos && retain)
            discovery_pub = true;
    EXPECT_TRUE(discovery_pub) << "HA discovery must be published on expose";

    // Command topics must now be subscribed
    std::string cmd_topic = "avionmesh/light/" + std::to_string(DEV2) + "/set";
    EXPECT_NE(hub.mqtt_subs.find(cmd_topic), hub.mqtt_subs.end())
        << "command topic must be subscribed after SetMqttExposed";
}

// Multiple actions pushed separately are all processed
TEST_F(ApiControlTest, MultipleActionsProcessedInOrder) {
    static constexpr uint16_t DEV2 = 32901;
    hub.db().add_device(DEV2, 90, "Light B");
    hub.test_setup();

    DeferredAction a1;
    a1.type = DeferredAction::Control; a1.id1 = DEV;  a1.brightness = 100; a1.color_temp = -1;
    DeferredAction a2;
    a2.type = DeferredAction::Control; a2.id1 = DEV2; a2.brightness = 50;  a2.color_temp = -1;
    hub.push_action(a1);
    hub.push_action(a2);

    EXPECT_EQ(hub.mesh_sends.size(), 2u);
    EXPECT_EQ(hub.mesh_sends[0].dest_id, DEV);
    EXPECT_EQ(hub.mesh_sends[1].dest_id, DEV2);
}
