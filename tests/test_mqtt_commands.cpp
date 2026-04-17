// Tests: MQTT JSON schema commands → BLE mesh send + MQTT state echo.
// LampDimmer (type=90) has dimming and color_temp.

#include "mock_hub.h"
#include "esphome/core/component.h"
#include <gtest/gtest.h>

using namespace avionmesh;

static constexpr uint16_t DEV = 32900;
static const std::string PREFIX = "avionmesh";

class MqttCommandTest : public ::testing::Test {
protected:
    TestHub hub;

    void SetUp() override {
        hub.db().add_device(DEV, 93 /* RecessedDL — has both dimming and color_temp */, "Test Light");
        hub.db().find_device(DEV)->mqtt_exposed = true;
        hub.test_setup();
        esphome::set_test_millis(1000);  // non-zero so rapid-dim window doesn't trigger
    }

    std::string cmd_topic() {
        return PREFIX + "/light/" + std::to_string(DEV) + "/set";
    }

    std::string state_topic() {
        return PREFIX + "/light/" + std::to_string(DEV) + "/state";
    }
};

// --- Brightness command (JSON) ---

TEST_F(MqttCommandTest, BrightnessSet_SendsToBle) {
    hub.inject_mqtt(cmd_topic(), R"({"brightness":128})");

    ASSERT_EQ(hub.mesh_sends.size(), 1u);
    auto &cmd = hub.mesh_sends[0];
    EXPECT_EQ(cmd.dest_id, DEV);
    EXPECT_EQ(cmd.payload[1], 0x0A);  // Noun::Dimming
    EXPECT_EQ(cmd.payload[5], 128);
}

TEST_F(MqttCommandTest, BrightnessSet_EchoesJsonStateToMqtt) {
    hub.inject_mqtt(cmd_topic(), R"({"brightness":200})");

    bool found = false;
    for (auto &[topic, payload, retain] : hub.mqtt_publishes) {
        if (topic == state_topic() && payload.find("\"brightness\":200") != std::string::npos) {
            found = true;
            EXPECT_NE(payload.find("\"state\":\"ON\""), std::string::npos);
        }
    }
    EXPECT_TRUE(found) << "expected JSON state echo on MQTT";
}

// --- ON command (JSON) ---

TEST_F(MqttCommandTest, OnCommand_NoPriorState_Sends255) {
    hub.inject_mqtt(cmd_topic(), R"({"state":"ON"})");

    ASSERT_EQ(hub.mesh_sends.size(), 1u);
    EXPECT_EQ(hub.mesh_sends[0].payload[5], 255);
}

TEST_F(MqttCommandTest, OnCommand_WithPriorBrightness_NoMeshSend) {
    hub.states()[DEV] = {128, 0, true, false};

    hub.inject_mqtt(cmd_topic(), R"({"state":"ON"})");

    // Device is already on at brightness 128 — no mesh send needed
    EXPECT_EQ(hub.mesh_sends.size(), 0u)
        << "ON when already on should not send a mesh command";
}

TEST_F(MqttCommandTest, OnCommand_PriorBrightnessIsZero_Sends255) {
    hub.states()[DEV] = {0, 0, true, false};

    hub.inject_mqtt(cmd_topic(), R"({"state":"ON"})");

    ASSERT_EQ(hub.mesh_sends.size(), 1u);
    EXPECT_EQ(hub.mesh_sends[0].payload[5], 255)
        << "ON with prior brightness=0 should fall back to 255";
}

// --- OFF command (JSON) ---

TEST_F(MqttCommandTest, OffCommand_SendsBrightness0) {
    hub.inject_mqtt(cmd_topic(), R"({"state":"OFF"})");

    ASSERT_EQ(hub.mesh_sends.size(), 1u);
    EXPECT_EQ(hub.mesh_sends[0].payload[5], 0);
}

TEST_F(MqttCommandTest, OffCommand_EchoesOffStateToMqtt) {
    hub.inject_mqtt(cmd_topic(), R"({"state":"OFF"})");

    bool found_off = false;
    for (auto &[topic, payload, retain] : hub.mqtt_publishes)
        if (topic == state_topic() && payload.find("\"state\":\"OFF\"") != std::string::npos)
            found_off = true;

    EXPECT_TRUE(found_off) << "expected JSON state echo with OFF";
}

// --- Color-temp command (JSON, atomic with state) ---

TEST_F(MqttCommandTest, ColorTempSet_SendsCorrectKelvin) {
    hub.inject_mqtt(cmd_topic(), R"({"state":"ON","color_temp":370})");

    // Should send color_temp command (brightness not sent because state=ON and device already off sends 255)
    bool found_color = false;
    for (auto &cmd : hub.mesh_sends) {
        if (cmd.payload[1] == 0x1D) {  // Noun::Color
            found_color = true;
            uint16_t kelvin = (static_cast<uint16_t>(cmd.payload[6]) << 8) | cmd.payload[7];
            EXPECT_EQ(kelvin, 1000000u / 370u);
        }
    }
    EXPECT_TRUE(found_color) << "expected color_temp mesh command";
}

TEST_F(MqttCommandTest, ColorTempOnly_DoesNotChangeBrightness) {
    hub.states()[DEV] = {128, 0, true, false};

    hub.inject_mqtt(cmd_topic(), R"({"state":"ON","color_temp":300})");

    // Should only send color_temp, no brightness command (device already on)
    for (auto &cmd : hub.mesh_sends) {
        EXPECT_NE(cmd.payload[1], 0x0A)
            << "color_temp change on already-on device should not send brightness command";
    }
    EXPECT_EQ(hub.states()[DEV].brightness, 128)
        << "brightness should remain unchanged";
}

// --- Rapid-dim detection (JSON) ---

TEST_F(MqttCommandTest, RapidDim_SecondCallWithinWindow_SkipsMeshSend) {
    esphome::set_test_millis(1000);
    hub.inject_mqtt(cmd_topic(), R"({"brightness":100})");
    ASSERT_EQ(hub.mesh_sends.size(), 1u);

    esphome::set_test_millis(1100);  // 100ms later — within 750ms window
    hub.inject_mqtt(cmd_topic(), R"({"brightness":80})");

    EXPECT_EQ(hub.mesh_sends.size(), 1u) << "rapid dim should suppress second mesh send";
}

TEST_F(MqttCommandTest, RapidDim_SecondCallOutsideWindow_SendsToBle) {
    esphome::set_test_millis(1000);
    hub.inject_mqtt(cmd_topic(), R"({"brightness":100})");

    esphome::set_test_millis(2000);  // 1s later — outside 750ms window
    hub.inject_mqtt(cmd_topic(), R"({"brightness":80})");

    EXPECT_EQ(hub.mesh_sends.size(), 2u) << "non-rapid dim should send both commands";
}
