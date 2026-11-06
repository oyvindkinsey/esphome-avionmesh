// Tests: initial-sync JSON batch builders on the hub.
//
// These payloads used to be inlined in avionmesh_web.cpp, which is not part
// of the host test build. Moving them onto AvionMeshHub keeps the wire
// format (state gating, batching, name escaping) testable.

#include "mock_hub.h"

#include <ArduinoJson.h>
#include <gtest/gtest.h>

using namespace avionmesh;

static const char *kNastyName = "café \"main\"\\lamp";

class SyncJsonTest : public ::testing::Test {
 protected:
    TestHub hub;

    void SetUp() override {
        // 134 (SmartBulb) is dimming + CT-capable, 90 is a plain dimmer,
        // 16 is a non-dimming outlet.
        hub.db().add_device(100, 134, kNastyName);
        hub.db().add_device(101, 90, "Lamp A");
        hub.db().add_device(102, 16, "Outlet");
        hub.db().add_group(256, "All \"kitchen\"\\");
        hub.db().add_device_to_group(100, 256);
        hub.test_setup();

        auto &s100 = hub.states()[100];
        s100.brightness = 200;
        s100.brightness_known = true;
        s100.color_temp = 340;
        s100.color_temp_known = true;

        auto &s101 = hub.states()[101];
        s101.brightness = 50;
        s101.brightness_known = true;
    }
};

TEST_F(SyncJsonTest, DevicesBatchValidAndStateGated) {
    JsonDocument doc;
    ASSERT_FALSE(deserializeJson(doc, hub.build_sync_devices_json(0, 5)));
    auto devs = doc["devices"];
    ASSERT_EQ(3u, devs.size());

    EXPECT_EQ(100u, devs[0]["avion_id"].as<uint32_t>());
    EXPECT_STREQ(devs[0]["name"].as<const char *>(), kNastyName);
    EXPECT_TRUE(devs[0]["has_dimming"].as<bool>());
    EXPECT_TRUE(devs[0]["has_color_temp"].as<bool>());
    EXPECT_EQ(200, devs[0]["brightness"].as<int>());
    EXPECT_EQ(340, devs[0]["color_temp"].as<int>());
    ASSERT_EQ(1u, devs[0]["groups"].size());
    EXPECT_EQ(256u, devs[0]["groups"][0].as<uint32_t>());

    // Brightness known but color temp not: no color_temp key.
    EXPECT_EQ(50, devs[1]["brightness"].as<int>());
    EXPECT_TRUE(devs[1]["color_temp"].isNull());
    EXPECT_FALSE(devs[1]["has_color_temp"].as<bool>());

    // Non-dimming device with no tracked state: no brightness key at all.
    EXPECT_TRUE(devs[2]["brightness"].isNull());
    EXPECT_FALSE(devs[2]["has_dimming"].as<bool>());
}

TEST_F(SyncJsonTest, BatchingClampsRange) {
    JsonDocument doc;
    ASSERT_FALSE(deserializeJson(doc, hub.build_sync_devices_json(0, 2)));
    ASSERT_EQ(2u, doc["devices"].size());

    ASSERT_FALSE(deserializeJson(doc, hub.build_sync_devices_json(2, 5)));
    ASSERT_EQ(1u, doc["devices"].size());
    EXPECT_EQ(102u, doc["devices"][0]["avion_id"].as<uint32_t>());

    ASSERT_FALSE(deserializeJson(doc, hub.build_sync_devices_json(99, 5)));
    ASSERT_EQ(0u, doc["devices"].size());
}

TEST_F(SyncJsonTest, GroupsBatchValid) {
    JsonDocument doc;
    ASSERT_FALSE(deserializeJson(doc, hub.build_sync_groups_json(0, 5)));
    auto grps = doc["groups"];
    ASSERT_EQ(1u, grps.size());
    EXPECT_EQ(256u, grps[0]["group_id"].as<uint32_t>());
    EXPECT_STREQ(grps[0]["name"].as<const char *>(), "All \"kitchen\"\\");
    ASSERT_EQ(1u, grps[0]["members"].size());
    EXPECT_EQ(100u, grps[0]["members"][0].as<uint32_t>());
    EXPECT_FALSE(grps[0]["mqtt_exposed"].as<bool>());
}
