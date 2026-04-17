// Tests: check_group_state_latch() — group brightness is latched via
// exclusive-witness + subset-propagation algorithm.

#include "mock_hub.h"
#include <gtest/gtest.h>

using namespace avionmesh;

static constexpr uint16_t DEV_A   = 32900;
static constexpr uint16_t DEV_B   = 32901;
static constexpr uint16_t GROUP_1 = 1024;

class GroupLatchTest : public ::testing::Test {
protected:
    TestHub hub;

    void SetUp() override {
        hub.db().add_device(DEV_A, 90, "Light A");
        hub.db().add_device(DEV_B, 90, "Light B");
        hub.db().add_group(GROUP_1, "Group 1");
        hub.db().add_device_to_group(DEV_A, GROUP_1);
        hub.db().add_device_to_group(DEV_B, GROUP_1);
        hub.test_setup();
    }
};

// A single-group device is its own exclusive witness — latches immediately
// without waiting for other members.
TEST_F(GroupLatchTest, SingleGroupDevice_LatchesImmediately) {
    hub.inject_brightness(DEV_A, 128);

    ASSERT_EQ(hub.states().count(GROUP_1), 1u);
    EXPECT_EQ(hub.states().at(GROUP_1).brightness, 128);

    bool group_sse = false;
    for (auto &[ev, data] : hub.sse_events)
        if (data.find("\"avion_id\":" + std::to_string(GROUP_1)) != std::string::npos)
            group_sse = true;
    EXPECT_TRUE(group_sse) << "SSE state event must be emitted for group on latch";
}

// Both devices report different brightnesses (sequential individual commands).
// Each exclusive-witness update latches the group; the last one wins.
TEST_F(GroupLatchTest, DivergentReports_LatestValueWins) {
    hub.inject_brightness(DEV_A, 128);
    hub.inject_brightness(DEV_B, 64);

    ASSERT_EQ(hub.states().count(GROUP_1), 1u);
    EXPECT_EQ(hub.states().at(GROUP_1).brightness, 64u)
        << "last exclusive-witness report wins";
}

// Both devices report the same brightness — group MUST latch.
TEST_F(GroupLatchTest, ConvergedBrightness_Latches) {
    hub.inject_brightness(DEV_A, 200);
    hub.inject_brightness(DEV_B, 200);

    ASSERT_EQ(hub.states().count(GROUP_1), 1u) << "group state must be created on latch";
    EXPECT_EQ(hub.states().at(GROUP_1).brightness, 200);
    EXPECT_TRUE(hub.states().at(GROUP_1).brightness_known);
}

// Convergence → SSE "state" event must be emitted for the group.
TEST_F(GroupLatchTest, ConvergedBrightness_EmitsGroupSseEvent) {
    hub.inject_brightness(DEV_A, 200);
    hub.inject_brightness(DEV_B, 200);

    bool found = false;
    for (auto &[ev, data] : hub.sse_events) {
        if (ev == "state" && data.find("\"avion_id\":" + std::to_string(GROUP_1)) != std::string::npos) {
            found = true;
            EXPECT_NE(data.find("\"brightness\":200"), std::string::npos);
        }
    }
    EXPECT_TRUE(found) << "expected SSE state event for group " << GROUP_1;
}

// When group is mqtt_exposed, convergence triggers MQTT state publishes.
TEST_F(GroupLatchTest, ConvergedBrightness_MqttExposed_PublishesMqtt) {
    hub.db().find_group(GROUP_1)->mqtt_exposed = true;
    hub.test_setup();  // re-subscribe so group command topics are registered

    hub.inject_brightness(DEV_A, 150);
    hub.inject_brightness(DEV_B, 150);

    bool found_brightness = false;
    for (auto &[topic, payload, retain] : hub.mqtt_publishes) {
        if (topic.find(std::to_string(GROUP_1)) != std::string::npos &&
            topic.find("/state") != std::string::npos &&
            payload.find("\"brightness\":150") != std::string::npos) {
            found_brightness = true;
        }
    }
    EXPECT_TRUE(found_brightness) << "expected JSON state MQTT publish for group with brightness 150";
}

// Each single-group update latches; after a second group command the group
// reflects the new brightness.
TEST_F(GroupLatchTest, SuccessiveGroupCommands_GroupTracksLatest) {
    hub.inject_brightness(DEV_A, 100);  // latch GROUP_1 = 100
    hub.inject_brightness(DEV_B, 100);  // same value, GROUP_1 stays 100

    hub.inject_brightness(DEV_A, 50);   // new command at 50
    hub.inject_brightness(DEV_B, 50);

    ASSERT_EQ(hub.states().count(GROUP_1), 1u);
    EXPECT_EQ(hub.states().at(GROUP_1).brightness, 50u);
}
