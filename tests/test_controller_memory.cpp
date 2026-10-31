#include "controller_memory.h"
#include "mock_hub.h"
#include <gtest/gtest.h>

using namespace avionmesh;

TEST(ControllerMemory, UnsupportedHostReportsNullInsteadOfInventedHeapOrStack) {
    const auto sample = sample_controller_memory(12345);
    JsonDocument document;
    write_controller_memory(document.to<JsonObject>(), sample);
    EXPECT_EQ(document["platform"].as<std::string>(), "unsupported");
    EXPECT_EQ(document["units"].as<std::string>(), "bytes");
    EXPECT_EQ(document["sampled_at_ms"].as<uint32_t>(), 12345u);
    for (const char *pool : {"internal", "psram"}) {
        EXPECT_FALSE(document[pool]["available"].as<bool>());
        EXPECT_EQ(document[pool]["unavailable_reason"].as<std::string>(), "unsupported_platform");
        for (const char *metric :
             {"total_bytes", "free_bytes", "minimum_free_bytes", "largest_free_block_bytes"})
            EXPECT_TRUE(document[pool][metric].isNull()) << pool << ":" << metric;
    }
    EXPECT_FALSE(document["task"]["available"].as<bool>());
    EXPECT_TRUE(document["task"]["name"].isNull());
    EXPECT_FALSE(document["task"]["stack_minimum_available"].as<bool>());
    EXPECT_TRUE(document["task"]["stack_minimum_free_bytes"].isNull());
}

TEST(ControllerMemory, SerializesDistinctByteMetricsAndEscapesTaskContext) {
    ControllerMemorySample sample;
    sample.platform = "synthetic_fixture";
    sample.internal = {true, nullptr, 100000, 40000, 23000, 12000};
    sample.psram = {true, nullptr, 8000000, 7000000, 6000000, 5000000};
    sample.task_available = sample.stack_minimum_available = true;
    sample.task_name = "test\"task\n";
    sample.stack_minimum_free_bytes = 4097;
    JsonDocument output;
    write_controller_memory(output.to<JsonObject>(), sample);
    std::string encoded;
    serializeJson(output, encoded);
    JsonDocument parsed;
    ASSERT_FALSE(deserializeJson(parsed, encoded));
    EXPECT_EQ(parsed["internal"]["free_bytes"].as<size_t>(), 40000u);
    EXPECT_EQ(parsed["internal"]["minimum_free_bytes"].as<size_t>(), 23000u);
    EXPECT_EQ(parsed["internal"]["largest_free_block_bytes"].as<size_t>(), 12000u);
    EXPECT_EQ(parsed["psram"]["free_bytes"].as<size_t>(), 7000000u);
    EXPECT_EQ(parsed["task"]["name"].as<std::string>(), sample.task_name);
    EXPECT_EQ(parsed["task"]["stack_minimum_free_bytes"].as<size_t>(), 4097u);
    EXPECT_EQ(parsed["minimum_scope"].as<std::string>(), "boot_lifetime_per_region_sum");
    EXPECT_EQ(parsed["task"]["stack_minimum_scope"].as<std::string>(), "task_lifetime");
}

class MemoryTestHub : public TestHub {
 public:
    void seed_runtime() {
        controller_dimmer_levels_[40002].observe(900, 1, 120, 10);
        controller_local_levels_[40001].observe(1, 197, 10, true);
        ControllerSlotRecords records{};
        for (uint8_t slot = 1; slot <= 4; ++slot)
            records[slot - 1] = {1, 0x30, 0, slot, 0xff, 0xff, 0, 0};
        ASSERT_TRUE(controller_bindings_.set(40003, 127, records));
        ASSERT_TRUE(controller_bindings_.set(40005, 126, records));
        ASSERT_TRUE(controller_bindings_.invalidate(40005));
        controller_slot_reads_[40003][0] = {"01300001ffff0000", 10};
        controller_slot_reads_[40003][2] = {"01300003ffff0000", 11};
        controller_slot_reads_[40006] = {};  // Retained map entry without completed reads.
        binding_refresh_status_[40003] = "ready";
        keypad_setup_status_[40003] = "ready";
        keypad_setup_status_[40005] = "waiting";
        keypad_setup_plan_.new_groups.push_back({900, "Synthetic target", {}, false});
        controller_events_published_ = 8;
        controller_events_duplicates_ = 3;
        uint8_t packet[] = {0x73, 1};
        controller_packets_.push(packet, sizeof(packet), 10);
        packet[1] = 2;
        controller_packets_.push(packet, sizeof(packet), 11);
        ControllerPacketQueue::Packet duplicate;
        duplicate.length = 2;
        duplicate.bytes[0] = 0x73;
        duplicate.bytes[1] = 3;
        ASSERT_TRUE(controller_duplicates_.accept(duplicate, 10));
        ASSERT_TRUE(capture_.start(0, 1000));
    }
    void status() { on_mqtt_command("{\"action\":\"controller_status\"}"); }
    std::vector<uint8_t> bindings_blob() const { return controller_bindings_.encode(); }
    void expect_runtime_preserved() {
        EXPECT_EQ(controller_events_published_, 8u);
        EXPECT_EQ(controller_events_duplicates_, 3u);
        ASSERT_EQ(controller_local_levels_.size(), 1u);
        EXPECT_EQ(controller_local_levels_.find(40001)->second.level(), 197);
        EXPECT_TRUE(controller_local_levels_.find(40001)->second.ready());
        ControllerPacketQueue::Packet packet;
        ASSERT_TRUE(controller_packets_.pop(packet));
        EXPECT_EQ(packet.received_ms, 10u);
        EXPECT_EQ(packet.bytes[1], 1);
        ASSERT_TRUE(controller_packets_.pop(packet));
        EXPECT_EQ(packet.received_ms, 11u);
        EXPECT_EQ(packet.bytes[1], 2);
        EXPECT_FALSE(controller_packets_.pop(packet));
        packet.length = 2;
        packet.bytes[0] = 0x73;
        packet.bytes[1] = 3;
        EXPECT_FALSE(controller_duplicates_.accept(packet, 20));
        // Reading status must not run the capture expiry transition either.
        EXPECT_TRUE(capture_.status(0).active);
    }
};

TEST(ControllerMemory, ManagementStatusReportsRetainedStateWithoutDrainingOrChangingIt) {
    MemoryTestHub hub;
    ASSERT_TRUE(hub.db().add_device(40001, 91, "Accessory", true));
    ASSERT_TRUE(hub.db().add_device(40002, 81, "Slider"));
    ASSERT_TRUE(hub.db().add_device(40003, 127, "Keypad"));
    // Comparable inventory count exercises load/controller counting separately.
    for (uint16_t id = 40100; id < 40178; ++id)
        ASSERT_TRUE(hub.db().add_device(id, 162, "Synthetic load"));
    ASSERT_TRUE(hub.db().add_group(900, "Protected target"));
    hub.db().find_group(900)->ha_event_target = true;
    hub.test_setup();
    hub.seed_runtime();
    const auto bindings_before = hub.bindings_blob();
    esphome::set_test_millis(5000);
    hub.status();
    hub.status();
    ASSERT_EQ(hub.mqtt_publishes.size(), 2u);
    for (const auto &[topic, encoded, retained] : hub.mqtt_publishes) {
        EXPECT_EQ(topic, "avionmesh/avionmesh/response");
        EXPECT_FALSE(retained);
        JsonDocument response;
        ASSERT_FALSE(deserializeJson(response, encoded));
        EXPECT_EQ(response["action"].as<std::string>(), "controller_status");
        EXPECT_EQ(response["published"].as<unsigned>(), 8u);
        EXPECT_EQ(response["duplicates"].as<unsigned>(), 3u);
        EXPECT_EQ(response["memory"]["sampled_at_ms"].as<uint32_t>(), 5000u);
        EXPECT_TRUE(response["memory"]["internal"]["free_bytes"].isNull());
        const auto storage = response["controller_storage"];
        EXPECT_EQ(storage["inventory_devices"].as<size_t>(), 81u);
        EXPECT_EQ(storage["inventory_controllers"].as<size_t>(), 3u);
        EXPECT_EQ(storage["inventory_groups"].as<size_t>(), 1u);
        EXPECT_EQ(storage["map_entries"]["absolute_dimmer_streams"].as<size_t>(), 1u);
        EXPECT_EQ(storage["map_entries"]["local_dimmer_streams"].as<size_t>(), 1u);
        EXPECT_EQ(storage["map_entries"]["slot_read_controllers"].as<size_t>(), 2u);
        EXPECT_EQ(storage["map_entries"]["keypad_setup_statuses"].as<size_t>(), 2u);
        EXPECT_EQ(storage["snapshots"]["slot_read_records"].as<size_t>(), 2u);
        EXPECT_EQ(storage["snapshots"]["bindings_retained"].as<size_t>(), 2u);
        EXPECT_EQ(storage["snapshots"]["bindings_valid"].as<size_t>(), 1u);
        EXPECT_GE(storage["snapshots"]["binding_vector_capacity"].as<size_t>(), 2u);
        EXPECT_EQ(storage["snapshots"]["setup_plan_groups"].as<size_t>(), 1u);
        EXPECT_EQ(storage["snapshots"]["setup_journal_groups"].as<size_t>(), 0u);
        EXPECT_EQ(storage["fixed_storage"]["packet_queue_object_bytes"].as<size_t>(),
                  sizeof(ControllerPacketQueue));
        EXPECT_EQ(storage["fixed_storage"]["duplicate_window_object_bytes"].as<size_t>(),
                  sizeof(ControllerDuplicateWindow));
        EXPECT_EQ(storage["fixed_storage"]["mesh_capture_object_bytes"].as<size_t>(),
                  sizeof(MeshCapture));
        EXPECT_EQ(storage["packet_queue"]["queued"].as<size_t>(), 2u);
        EXPECT_EQ(response["local_dimmers"][0]["brightness"].as<unsigned>(), 197u);
    }
    EXPECT_EQ(hub.bindings_blob(), bindings_before);
    EXPECT_EQ(hub.db().devices().size(), 81u);
    EXPECT_TRUE(hub.db().find_group(900)->ha_event_target);
    EXPECT_TRUE(hub.db().find_group(900)->member_ids.empty());
    EXPECT_TRUE(hub.mesh_sends.empty());
    EXPECT_TRUE(hub.sse_events.empty());
    hub.expect_runtime_preserved();
    esphome::set_test_millis(0);
}
