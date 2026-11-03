// Tests: BLE mesh RX → SSE "state" events.

#include "mock_hub.h"
#include <gtest/gtest.h>

using namespace avionmesh;

static constexpr uint16_t DEV_A = 32900;
static constexpr uint16_t DEV_B = 32901;
static constexpr uint16_t GROUP_1 = 1024;

class SseEventTest : public ::testing::Test {
 protected:
    TestHub hub;

    void SetUp() override {
        hub.db().add_device(DEV_A, 90, "Light A");
        hub.test_setup();
    }
};

TEST_F(SseEventTest, BrightnessBleRx_EmitsStateEvent) {
    hub.inject_brightness(DEV_A, 128);

    ASSERT_EQ(hub.sse_events.size(), 1u);
    EXPECT_EQ(hub.sse_events[0].first, "state");
    EXPECT_NE(hub.sse_events[0].second.find("\"avion_id\":" + std::to_string(DEV_A)),
              std::string::npos);
    EXPECT_NE(hub.sse_events[0].second.find("\"brightness\":128"), std::string::npos);
}

TEST_F(SseEventTest, ColorTempBleRx_EmitsStateEventWithColorTemp) {
    hub.inject_brightness(DEV_A, 200);
    hub.inject_color_temp(DEV_A, 3000);
    hub.clear_captures();

    // Re-inject color temp update; prior brightness state is known from inject_brightness
    hub.inject_color_temp(DEV_A, 4000);

    ASSERT_EQ(hub.sse_events.size(), 1u);
    EXPECT_EQ(hub.sse_events[0].first, "state");
    EXPECT_NE(hub.sse_events[0].second.find("\"color_temp\":4000"), std::string::npos);
}

TEST_F(SseEventTest, UnknownDevice_NoSseEvent) {
    // Device 40000 not in DB — on_mesh_rx returns early
    hub.inject_brightness(40000, 128);
    EXPECT_TRUE(hub.sse_events.empty());
}

TEST_F(SseEventTest, GroupLatch_EmitsGroupSseEvent) {
    hub.db().add_device(DEV_B, 90, "Light B");
    hub.db().add_group(GROUP_1, "Group 1");
    hub.db().add_device_to_group(DEV_A, GROUP_1);
    hub.db().add_device_to_group(DEV_B, GROUP_1);
    hub.test_setup();

    hub.inject_brightness(DEV_A, 100);
    hub.inject_brightness(DEV_B, 100);

    bool group_event = false;
    for (auto &[ev, data] : hub.sse_events) {
        if (ev == "state" &&
            data.find("\"avion_id\":" + std::to_string(GROUP_1)) != std::string::npos)
            group_event = true;
    }
    EXPECT_TRUE(group_event) << "group latch must emit SSE state for group";
}

TEST_F(SseEventTest, StateJsonFormat_NoColorTemp) {
    hub.inject_brightness(DEV_A, 77);

    ASSERT_FALSE(hub.sse_events.empty());
    auto &json = hub.sse_events.back().second;
    // Must contain avion_id and brightness; must NOT contain color_temp before CT is known
    EXPECT_NE(json.find("\"brightness\":77"), std::string::npos);
    EXPECT_EQ(json.find("color_temp"), std::string::npos)
        << "color_temp must not appear until a CT value is known";
}

TEST_F(SseEventTest, StateJsonFormat_WithColorTemp) {
    hub.inject_brightness(DEV_A, 77);
    hub.clear_captures();
    hub.inject_color_temp(DEV_A, 3000);  // CT becomes known

    ASSERT_FALSE(hub.sse_events.empty());
    auto &json = hub.sse_events.back().second;
    EXPECT_NE(json.find("\"color_temp\":3000"), std::string::npos);
}

TEST_F(SseEventTest, RepeatedReport_NoDuplicateStateEvent) {
    hub.inject_brightness(DEV_A, 128);
    hub.clear_captures();

    hub.inject_brightness(DEV_A, 128);  // mesh re-reports the same value

    EXPECT_TRUE(hub.sse_events.empty()) << "unchanged state must not be re-sent";
}

TEST_F(SseEventTest, RepeatedReport_ChangeStillEmitted) {
    hub.inject_brightness(DEV_A, 128);
    hub.inject_brightness(DEV_A, 128);
    hub.clear_captures();

    hub.inject_brightness(DEV_A, 0);

    ASSERT_EQ(hub.sse_events.size(), 1u);
    EXPECT_NE(hub.sse_events[0].second.find("\"brightness\":0"), std::string::npos);
}

TEST_F(SseEventTest, RepeatedReport_NoDuplicateGroupStateEvent) {
    hub.db().add_device(DEV_B, 90, "Light B");
    hub.db().add_group(GROUP_1, "Group 1");
    hub.db().add_device_to_group(DEV_A, GROUP_1);
    hub.db().add_device_to_group(DEV_B, GROUP_1);
    hub.test_setup();

    hub.inject_brightness(DEV_A, 100);
    hub.inject_brightness(DEV_B, 100);
    hub.clear_captures();

    // A full mesh read re-reports both lights; each report re-latches the group
    hub.inject_brightness(DEV_A, 100);
    hub.inject_brightness(DEV_B, 100);

    EXPECT_TRUE(hub.sse_events.empty()) << "re-latching an unchanged group must not re-send it";
}

TEST_F(SseEventTest, RepeatedReport_RetriedAfterDroppedEvent) {
    hub.sse_accepts = false;            // a client's send buffer is busy
    hub.inject_brightness(DEV_A, 128);  // this event doesn't reach it
    hub.sse_accepts = true;
    hub.clear_captures();

    hub.inject_brightness(DEV_A, 128);  // the next identical report

    ASSERT_EQ(hub.sse_events.size(), 1u) << "a dropped state must be re-sent on the next report";
    EXPECT_NE(hub.sse_events[0].second.find("\"brightness\":128"), std::string::npos);
}
