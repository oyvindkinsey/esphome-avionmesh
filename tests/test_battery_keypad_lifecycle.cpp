#include "mock_hub.h"
#include <ArduinoJson.h>
#include <gtest/gtest.h>

using namespace avionmesh;
namespace {
constexpr uint16_t BATTERY = 40026;
constexpr uint8_t WAKE[] = {0x0b, 0x1c, 1, 0x0b, 5, 100, 1};
const ControllerSlotRecords BASELINE{{{{1, 0x30, 0, 1, 2, 0xf4, 8, 255}},
                                      {{1, 0x30, 0, 2, 255, 255, 0, 0}},
                                      {{1, 0x30, 0, 3, 255, 255, 0, 0}},
                                      {{1, 0x30, 0, 4, 255, 255, 0, 0}}}};
class BatteryHub : public TestHub {
 public:
    BatteryHub() {
        esphome::set_test_millis(1000);
        db_.set_passphrase("synthetic-battery-mesh");
        controller_bindings_.load(db_.passphrase());
        db_.add_device(BATTERY, 126, "Battery", true);
        db_.add_device(40027, 127, "Wired", true);
        db_.add_device(40028, 162, "Load", true);
        mesh_initialized_ = crypto_initialized_ = true;
        ble_state_ = BleState::Ready;
        test_setup();
    }
    using AvionMeshHub::controller_bindings_json;
    using AvionMeshHub::keypad_setup_status;
    using AvionMeshHub::on_mqtt_command;
    using AvionMeshHub::start_controller_binding_refresh;
    using AvionMeshHub::start_keypad_setup;
    void step(uint32_t now) {
        esphome::set_test_millis(now);
        process_controller_binding_refresh();
    }
    void wake(uint16_t source = BATTERY, uint32_t received = 1000) {
        handle_battery_keypad_wake(source, 0x73, WAKE, sizeof(WAKE), received);
    }
    void reply(const uint8_t *payload, size_t length) {
        report_controller_read("reply", payload, length);
    }
    void timeout() { report_controller_read("timeout"); }
    void next_wake() {
        const uint32_t now = esphome::millis() + 1000;
        esphome::set_test_millis(now);
        wake(BATTERY, now);
        step(now + 150);
    }
    bool receive(const uint8_t *payload, size_t length, uint32_t received,
                 uint16_t source = BATTERY) {
        if (!controller_read_reply_is_current(source, 0x73, payload, length, received))
            return false;
        report_controller_read("reply", payload, length);
        return true;
    }
    void ordinary_on(uint32_t received) {
        const uint8_t on[]{0, 10, 2, 0xf4, 0, 255, 0, 0, 0, 0};
        handle_battery_keypad_wake(BATTERY, 0x73, on, sizeof(on), received);
    }
    void tick(uint32_t now) {
        esphome::set_test_millis(now);
        loop();
    }
    void disconnected() { mesh_initialized_ = false; }
    bool reading() const { return controller_read_active_; }
    bool refreshing() const { return binding_refresh_active_; }
    uint32_t timeout_ms() const { return controller_read_timeout_ms(); }
    const ControllerBindingSnapshot *snapshot() const { return controller_bindings_.find(BATTERY); }
    std::vector<uint8_t> saved() const { return controller_bindings_.encode(); }
    bool restore(const std::vector<uint8_t> &blob) {
        return controller_bindings_.decode(blob, db_.passphrase());
    }
    void feed_round(uint8_t assigned = 1, const ControllerSlotRecords &records = BASELINE) {
        const uint8_t count[]{4, 0x30, 0, assigned, 4};
        step(esphome::millis());
        ASSERT_TRUE(reading());
        reply(count, sizeof(count));
        for (const auto &record : records) {
            step(esphome::millis());
            ASSERT_TRUE(reading());
            reply(record.data(), record.size());
        }
    }
    JsonDocument status() {
        JsonDocument doc;
        EXPECT_FALSE(deserializeJson(doc, controller_bindings_json(BATTERY)));
        return doc;
    }
};
}  // namespace

TEST(BatteryKeypad, ReadRequiresRequestedAuthenticatedSourceWakeAndSettles) {
    BatteryHub hub;
    hub.start_controller_binding_refresh(BATTERY);
    hub.step(1100);
    hub.wake(40027, 1100);   // Wrong product.
    hub.wake(40028, 1100);   // A load never arms battery mapping.
    hub.wake(BATTERY, 999);  // Queued before request, despite packet freshness.
    hub.step(1200);
    EXPECT_TRUE(hub.mesh_sends.empty());
    EXPECT_EQ(hub.status()["status"], "waiting_for_wake");
    hub.wake(BATTERY, 1200);
    hub.step(1349);
    EXPECT_TRUE(hub.mesh_sends.empty());
    hub.step(1350);
    ASSERT_EQ(hub.mesh_sends.size(), 1u);
    EXPECT_EQ(hub.mesh_sends[0].payload[0], 4);  // COUNT only.
    EXPECT_EQ(hub.timeout_ms(), 750u);
}

TEST(BatteryKeypad, WakeTimeoutAndSleepRetriesNeverWriteOrReserveGroups) {
    BatteryHub hub;
    hub.start_controller_binding_refresh(BATTERY);
    esphome::set_test_millis(181000);
    hub.wake(BATTERY, 181000);  // A fresh packet cannot reopen the expired request.
    hub.step(181000);
    EXPECT_FALSE(hub.refreshing());
    EXPECT_EQ(hub.status()["status"], "wake_timeout_try_read_again");
    EXPECT_TRUE(hub.mesh_sends.empty());
    hub.start_controller_binding_refresh(BATTERY);
    hub.wake(BATTERY, 181000);
    hub.step(181150);
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        ASSERT_TRUE(hub.reading());
        hub.timeout();
        hub.step(esphome::millis());
        if (attempt < 2) {
            EXPECT_FALSE(hub.reading());
            EXPECT_EQ(hub.status()["status"], "waiting_for_wake");
            EXPECT_EQ(hub.status()["wake_reason"], "read_timeout_restart_round");
            hub.next_wake();
        }
    }
    EXPECT_FALSE(hub.refreshing());
    EXPECT_EQ(hub.status()["status"], "read_failed");
    ASSERT_EQ(hub.mesh_sends.size(), 3u);  // Initial read plus two retries.
    for (const auto &command : hub.mesh_sends) {
        EXPECT_EQ(command.dest_id, BATTERY);
        EXPECT_EQ(command.payload[0], 4);
        EXPECT_EQ(command.payload[1], 0x30);
    }
    EXPECT_TRUE(hub.db().groups().empty());
    hub.step(esphome::millis() + 10000);  // No silent auto-resume after sleep/timeouts.
    EXPECT_EQ(hub.mesh_sends.size(), 3u);
}

TEST(BatteryKeypad, MatchingFullBaselinePersistsNativeButtonButCannotEnableProgramming) {
    BatteryHub hub;
    hub.start_controller_binding_refresh(BATTERY);
    hub.wake();
    hub.step(1150);
    hub.feed_round();
    hub.next_wake();
    hub.feed_round();
    ASSERT_TRUE(hub.snapshot() && hub.snapshot()->valid);
    EXPECT_EQ(hub.snapshot()->records, BASELINE);
    EXPECT_EQ(hub.status()["status"], "saved");
    EXPECT_EQ(hub.keypad_setup_status(BATTERY), "programming_unverified");
    hub.start_keypad_setup(BATTERY, true);
    EXPECT_EQ(hub.keypad_setup_status(BATTERY), "programming_unverified");
    EXPECT_FALSE(hub.db().find_device(BATTERY)->ha_setup_pending);
    EXPECT_TRUE(hub.db().groups().empty());
    ASSERT_EQ(hub.mesh_sends.size(), 10u);
    for (const auto &command : hub.mesh_sends)
        EXPECT_TRUE(command.payload[0] == 1 || command.payload[0] == 4);
    BatteryHub restarted;
    ASSERT_TRUE(restarted.restore(hub.saved()));
    EXPECT_EQ(restarted.snapshot()->records, BASELINE);
    EXPECT_EQ(restarted.keypad_setup_status(BATTERY), "programming_unverified");
    EXPECT_TRUE(restarted.mesh_sends.empty());
    restarted.start_controller_binding_refresh(BATTERY);
    EXPECT_FALSE(restarted.snapshot()->valid);
    restarted.step(1200);
    EXPECT_TRUE(restarted.mesh_sends.empty());  // New read needs a new wake.
}

TEST(BatteryKeypad, MatchingRoundsWithCountMismatchCannotSaveMapping) {
    BatteryHub hub;
    hub.start_controller_binding_refresh(BATTERY);
    hub.wake();
    hub.step(1150);
    hub.feed_round(0);
    hub.next_wake();
    hub.feed_round(0);
    EXPECT_FALSE(hub.snapshot() && hub.snapshot()->valid);
    EXPECT_EQ(hub.status()["status"], "count_does_not_match_assignments");
}

TEST(BatteryKeypad, ChangingBaselineCannotSaveMapping) {
    BatteryHub hub;
    hub.start_controller_binding_refresh(BATTERY);
    hub.wake();
    hub.step(1150);
    hub.feed_round();
    auto changed = BASELINE;
    changed[0][5] ^= 1;
    hub.next_wake();
    hub.feed_round(1, changed);
    EXPECT_FALSE(hub.snapshot() && hub.snapshot()->valid);
    EXPECT_EQ(hub.status()["status"], "assignments_changed_during_read");
}

TEST(BatteryKeypad, DisconnectModelRemovalAndMeshChangeCancelOutstandingRead) {
    for (unsigned change = 0; change < 4; ++change) {
        BatteryHub hub;
        hub.start_controller_binding_refresh(BATTERY);
        hub.wake();
        hub.step(1150);
        ASSERT_TRUE(hub.reading());
        if (change == 0)
            hub.disconnected();
        if (change == 1)
            hub.db().find_device(BATTERY)->product_type = 127;
        if (change == 2)
            hub.db().set_passphrase("another-mesh");
        if (change == 3)
            hub.db().remove_device(BATTERY);
        hub.step(1200);
        EXPECT_FALSE(hub.refreshing());
        EXPECT_FALSE(hub.reading());
        EXPECT_FALSE(hub.snapshot() && hub.snapshot()->valid);
        EXPECT_EQ(hub.mesh_sends.size(), 1u);
    }
}

TEST(BatteryKeypad, ActualLoopUsesShortDeadlineThenWaitsForWakeInsteadOfRetryingAsleep) {
    BatteryHub hub;
    hub.start_controller_binding_refresh(BATTERY);
    hub.wake();
    hub.step(1150);
    hub.tick(1899);
    EXPECT_EQ(hub.mesh_sends.size(), 1u);
    hub.tick(1900);
    EXPECT_EQ(hub.mesh_sends.size(), 1u);
    EXPECT_FALSE(hub.reading());
    EXPECT_EQ(hub.status()["status"], "waiting_for_wake");
    hub.tick(5000);
    EXPECT_EQ(hub.mesh_sends.size(), 1u);
    hub.ordinary_on(5000);
    hub.tick(5100);
    EXPECT_EQ(hub.mesh_sends.size(), 1u);
    hub.next_wake();
    EXPECT_EQ(hub.mesh_sends.size(), 2u);
    EXPECT_TRUE(hub.reading());
    EXPECT_EQ(hub.mesh_sends.back().payload[0], 4); // Restart at COUNT.
}

TEST(BatteryKeypad, WakeRequestAndSettlingSurviveMillisWrap) {
    BatteryHub hub;
    esphome::set_test_millis(UINT32_MAX - 50);
    hub.start_controller_binding_refresh(BATTERY);
    esphome::set_test_millis(49);
    hub.wake(BATTERY, 49);
    hub.step(198);
    EXPECT_TRUE(hub.mesh_sends.empty());
    hub.step(199);
    EXPECT_EQ(hub.mesh_sends.size(), 1u);
}

TEST(BatteryKeypad, MalformedCountAndSlotDoNotBecomeEmptyBaseline) {
    for (unsigned malformed = 0; malformed < 2; ++malformed) {
        BatteryHub hub;
        hub.start_controller_binding_refresh(BATTERY);
        hub.wake();
        hub.step(1150);
        uint8_t count[]{4, 0x30, 0, 1, 4};
        if (malformed == 0)
            count[3] = 5;
        hub.reply(count, sizeof(count));
        if (malformed == 1) {
            hub.step(1150);
            auto wrong_slot = BASELINE[0];
            wrong_slot[3] = 2;
            hub.reply(wrong_slot.data(), wrong_slot.size());
        }
        EXPECT_FALSE(hub.refreshing());
        EXPECT_FALSE(hub.snapshot() && hub.snapshot()->valid);
        EXPECT_TRUE(hub.db().groups().empty());
    }
}

TEST(BatteryKeypad, FourReadyNativeSlotsStillReportUnsupportedProgramming) {
    BatteryHub hub;
    auto full = BASELINE;
    for (unsigned slot = 1; slot < 4; ++slot) {
        full[slot][4] = 3;
        full[slot][5] = slot;
        full[slot][6] = 8;
        full[slot][7] = 255;
    }
    hub.start_controller_binding_refresh(BATTERY);
    hub.wake();
    hub.step(1150);
    hub.feed_round(4, full);
    hub.next_wake();
    hub.feed_round(4, full);
    auto status = hub.status();
    EXPECT_EQ(status["setup_status"], "programming_unverified");
    EXPECT_EQ(status["assignment_read_supported"], true);
    EXPECT_EQ(status["assignment_write_supported"], false);
    EXPECT_EQ(status["wake_required"], true);
    for (const auto &slot : status["slots"].as<JsonArray>())
        EXPECT_EQ(slot["status"], "ready");
}

TEST(BatteryKeypad, DirectProgrammingRemainsRejectedAfterFreshReadAndOwnedEmptyTarget) {
    BatteryHub hub;
    hub.start_controller_binding_refresh(BATTERY);
    hub.wake();
    hub.step(1150);
    hub.feed_round();
    hub.next_wake();
    hub.feed_round();
    ASSERT_TRUE(hub.db().add_group(24574, "HA event target: 40026:2"));
    hub.clear_captures();
    hub.on_mqtt_command(
        R"({"action":"controller_program","target":40026,"slot":2,"group_id":24574,"expected_record":"01300002ffff0000","request_id":99})");
    EXPECT_TRUE(hub.mesh_sends.empty());
    ASSERT_FALSE(hub.mqtt_publishes.empty());
    bool rejected = false;
    for (const auto &[topic, payload, retained] : hub.mqtt_publishes) {
        JsonDocument doc;
        ASSERT_FALSE(deserializeJson(doc, payload));
        if (doc["action"] == "controller_program") {
            rejected = doc["status"] == "error";
            EXPECT_EQ(doc["message"], "programming_unverified");
        }
    }
    EXPECT_TRUE(rejected);
    EXPECT_EQ(hub.snapshot()->records, BASELINE);
}

TEST(BatteryKeypad, ObservedFirstRoundTimingWaitsForSecondWakeAndRejectsOldReplies) {
    BatteryHub hub;
    hub.start_controller_binding_refresh(BATTERY);
    EXPECT_FALSE(hub.receive(BASELINE[0].data(), 8, 999)); // Pre-wake READ-shaped packet.
    hub.wake();
    hub.step(1150);
    EXPECT_FALSE(hub.receive(BASELINE[1].data(), 8, 1000)); // Wrong shape for current COUNT.
    const uint8_t count[]{4, 0x30, 0, 1, 4};
    esphome::set_test_millis(1538);
    ASSERT_TRUE(hub.receive(count, sizeof(count), 1538));
    const uint32_t times[]{1838, 2138, 2378, 2558};
    for (unsigned slot = 0; slot < 4; ++slot) {
        hub.step(esphome::millis());
        esphome::set_test_millis(times[slot]);
        ASSERT_TRUE(hub.receive(BASELINE[slot].data(), 8, times[slot]));
    }
    EXPECT_EQ(hub.status()["status"], "waiting_for_wake");
    EXPECT_EQ(hub.status()["round"], 2);
    EXPECT_EQ(hub.status()["wake_reason"], "verification_round");
    EXPECT_EQ(hub.mesh_sends.size(), 5u);
    EXPECT_FALSE(hub.snapshot() && hub.snapshot()->valid);
    hub.step(3000);
    hub.wake(BATTERY, 2500); // Fresh enough, but before the second wake wait.
    hub.step(3200);
    EXPECT_EQ(hub.mesh_sends.size(), 5u);
    EXPECT_FALSE(hub.receive(BASELINE[1].data(), 8, 3200)); // Late old reply while idle.
    hub.ordinary_on(3200);
    hub.step(3400);
    EXPECT_EQ(hub.mesh_sends.size(), 5u);
    hub.next_wake();
    ASSERT_EQ(hub.mesh_sends.size(), 6u);
    EXPECT_FALSE(hub.receive(BASELINE[1].data(), 8, esphome::millis())); // Old slot vs new COUNT.
    EXPECT_FALSE(hub.receive(count, 5, 1538)); // Queued before current request.
    EXPECT_FALSE(hub.receive(count, 5, esphome::millis(), 40027)); // Foreign source.
    hub.feed_round();
    ASSERT_TRUE(hub.snapshot() && hub.snapshot()->valid);
    EXPECT_EQ(hub.snapshot()->records, BASELINE);
    EXPECT_EQ(hub.mesh_sends.size(), 10u);
}

TEST(BatteryKeypad, TimeoutDiscardsPartialVerificationRoundAndRestartsAtCountAfterWake) {
    BatteryHub hub;
    hub.start_controller_binding_refresh(BATTERY);
    hub.wake();
    hub.step(1150);
    hub.feed_round();
    hub.next_wake();
    const uint8_t count[]{4, 0x30, 0, 1, 4};
    hub.reply(count, sizeof(count));
    hub.step(esphome::millis());
    auto changed = BASELINE[0];
    changed[5] ^= 1;
    hub.reply(changed.data(), changed.size());
    hub.step(esphome::millis()); // Slot 2 now pending when the receive window ends.
    hub.timeout();
    EXPECT_FALSE(hub.reading());
    EXPECT_EQ(hub.status()["query_slot"], 0);
    EXPECT_EQ(hub.status()["round"], 2);
    EXPECT_EQ(hub.status()["wake_retries"], 1);
    EXPECT_FALSE(hub.snapshot() && hub.snapshot()->valid);
    hub.next_wake();
    EXPECT_EQ(hub.mesh_sends.back().payload[0], 4);
    hub.feed_round(); // Must read COUNT and every slot again, replacing discarded partial data.
    ASSERT_TRUE(hub.snapshot() && hub.snapshot()->valid);
    EXPECT_EQ(hub.snapshot()->records, BASELINE);
    EXPECT_TRUE(hub.db().groups().empty());
}

TEST(BatteryKeypad, SecondWakeCannotExtendOriginalSessionDeadlineOrResumeAfterRestart) {
    BatteryHub hub;
    hub.start_controller_binding_refresh(BATTERY);
    hub.wake();
    hub.step(1150);
    hub.feed_round();
    esphome::set_test_millis(180800);
    hub.wake(BATTERY, 180800);
    hub.step(180950);
    ASSERT_TRUE(hub.reading()); // A request can start near the original deadline.
    const uint8_t count[]{4, 0x30, 0, 1, 4};
    esphome::set_test_millis(181000);
    EXPECT_FALSE(hub.receive(count, sizeof(count), 181000)); // No late finalization past 180s.
    hub.step(181000);
    EXPECT_FALSE(hub.refreshing());
    EXPECT_FALSE(hub.reading());
    EXPECT_EQ(hub.status()["status"], "read_session_timeout_try_again");
    EXPECT_EQ(hub.mesh_sends.size(), 6u);
    hub.wake(BATTERY, 181000);
    hub.step(182000);
    EXPECT_EQ(hub.mesh_sends.size(), 6u);
    BatteryHub restarted;
    restarted.db().find_device(BATTERY)->ha_setup_pending = true; // Historical stored trial flag.
    restarted.tick(2000);
    EXPECT_FALSE(restarted.refreshing());
    EXPECT_TRUE(restarted.mesh_sends.empty());
}
