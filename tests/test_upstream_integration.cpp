// Regression checks for upstream rename/bulk-import changes with controllers.
#include "mock_hub.h"
#include <ArduinoJson.h>
#include <gtest/gtest.h>

using namespace avionmesh;

TEST(UpstreamIntegration, RenameControllerKeepsEventDiscoveryAndEscapesName) {
    TestHub hub;
    ASSERT_TRUE(hub.db().add_device(33001, 91, "Accessory"));
    hub.test_setup();
    DeferredAction action;
    action.type = DeferredAction::RenameDevice;
    action.id1 = 33001;
    action.name = "Office \"dimmer\"";
    hub.push_action(action);
    bool saw_event = false;
    for (const auto &[topic, payload, retained] : hub.mqtt_publishes) {
        EXPECT_FALSE(topic.find("homeassistant/light/") == 0 && !payload.empty());
        if (topic.find("homeassistant/event/") == 0 && !payload.empty()) {
            JsonDocument doc;
            ASSERT_FALSE(deserializeJson(doc, payload));
            EXPECT_EQ(doc["device"]["name"].as<std::string>(), action.name);
            saw_event = true;
        }
    }
    EXPECT_TRUE(saw_event);
    ASSERT_EQ(hub.sse_events.size(), 1u);
    JsonDocument event;
    ASSERT_FALSE(deserializeJson(event, hub.sse_events[0].second));
    EXPECT_EQ(event["name"].as<std::string>(), action.name);
    EXPECT_TRUE(hub.mesh_sends.empty());
}

TEST(UpstreamIntegration, RenamePreservesProtectedTargetOwnership) {
    TestHub hub;
    ASSERT_TRUE(hub.db().add_group(1024, "HA event target: 33001:2"));
    ASSERT_TRUE(hub.db().add_group(1025, "Protected slider target"));
    hub.db().find_group(1025)->ha_event_target = true;
    for (uint16_t id : {1024, 1025}) {
        DeferredAction action;
        action.type = DeferredAction::RenameGroup;
        action.id1 = id;
        action.name = "Renamed";
        hub.push_action(action);
        EXPECT_NE(hub.db().find_group(id)->name, action.name);
    }
    EXPECT_TRUE(hub.sse_events.empty());
    EXPECT_TRUE(hub.mesh_sends.empty());
    ASSERT_TRUE(hub.db().add_group(1026, "Normal group"));
    EXPECT_TRUE(hub.db().rename_group(1026, "Normal renamed"));
}

TEST(UpstreamIntegration, MetadataOnlyBulkImportPreservesControllerSettingsAndTargets) {
    TestHub hub;
    ASSERT_TRUE(hub.db().add_device(33001, 91, "Existing accessory", true));
    ASSERT_TRUE(hub.db().add_group(1024, "HA event target: 33001:2"));
    hub.test_setup();
    JsonDocument doc;
    doc["metadata_only"] = true;
    auto devices = doc["devices"].to<JsonArray>();
    for (unsigned i = 0; i < 81; ++i) {
        auto device = devices.add<JsonObject>();
        device["device_id"] = 33001 + i;
        device["name"] = "Synthetic " + std::to_string(i);
        device["product_type"] = 162;
    }
    auto groups = doc["groups"].to<JsonArray>();
    for (unsigned i = 0; i < 16; ++i) {
        auto group = groups.add<JsonObject>();
        group["group_id"] = 1024 + i;
        group["name"] = "Group " + std::to_string(i);
        group["members"].to<JsonArray>().add(33001 + i);
    }
    DeferredAction action;
    action.type = DeferredAction::Import;
    serializeJson(doc, action.body);
    hub.push_action(action);
    EXPECT_EQ(hub.db().devices().size(), 81u);
    EXPECT_EQ(hub.db().groups().size(), 16u);
    EXPECT_EQ(hub.db().find_device(33001)->name, "Existing accessory");
    EXPECT_TRUE(hub.db().find_device(33001)->ha_local_level);
    EXPECT_EQ(hub.db().find_group(1024)->name, "HA event target: 33001:2");
    EXPECT_TRUE(hub.db().find_group(1024)->member_ids.empty());
    EXPECT_TRUE(hub.db().find_device(33001)->groups.empty());
    EXPECT_EQ(hub.db().find_group(1025)->member_ids, std::vector<uint16_t>({33002}));
    EXPECT_TRUE(hub.mesh_sends.empty());
}
