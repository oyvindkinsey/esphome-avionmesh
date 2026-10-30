#include "mqtt_discovery.h"
#include <ArduinoJson.h>
#include <cassert>
#include <tuple>
#include <vector>

int main() {
    avionmesh::MqttDiscovery discovery;
    discovery.set_node_name("test-bridge");
    discovery.set_topic_prefix("test_bridge");
    std::vector<std::tuple<std::string, std::string, bool>> messages;
    discovery.set_publish_fn([&](const std::string &topic, const std::string &payload,
                                 bool retain) { messages.emplace_back(topic, payload, retain); });
    discovery.publish_controller(40001, "Keypad \"A\"", "Wired Scene Keypad");
    assert(messages.size() == 1 && std::get<2>(messages[0]));
    assert(std::get<0>(messages[0]) == "homeassistant/event/test-bridge_40001/config");
    JsonDocument doc;
    assert(!deserializeJson(doc, std::get<1>(messages[0])));
    assert(doc["state_topic"] == "test_bridge/controller/40001/event");
    assert(doc["unique_id"] == "test-bridge_40001_commands");
    assert(doc["device"]["name"] == "Keypad \"A\"");
    assert(doc["availability_mode"] == "all");
    assert(doc["availability"].size() == 2);
    assert(doc["availability"][0]["topic"] == "test_bridge/status");
    assert(doc["availability"][1]["topic"] == "test_bridge/controller/availability");
    assert(doc["event_types"].size() == 6 && doc["event_types"][3] == "raise");
    assert(doc["command_topic"].isNull());
    avionmesh::ControllerBindingSnapshot snapshot{40001,
                                                  127,
                                                  true,
                                                  {{{{1, 0x30, 0, 1, 4, 0xb0, 8, 255}},
                                                    {{1, 0x30, 0, 2, 0x5f, 0xfe, 8, 255}},
                                                    {{1, 0x30, 0, 3, 0x5f, 0xfd, 8, 255}},
                                                    {{1, 0x30, 0, 4, 0xff, 0xff, 0, 0}}}}};
    messages.clear();
    discovery.publish_controller_actions(40001, 127, "Keypad \"A\"", &snapshot);
    unsigned event_configs = 0, trigger_configs = 0;
    bool button4_offline = false, button4_trigger_removed = false;
    for (const auto &[topic, payload, retained] : messages) {
        assert(retained);
        if (topic == "test_bridge/controller/40001/action/button_4/availability")
            button4_offline = payload == "offline";
        if (topic == "homeassistant/device_automation/test-bridge_40001_button_4/config")
            button4_trigger_removed = payload.empty();
        if (topic.rfind("homeassistant/", 0) != 0 || payload.empty())
            continue;
        assert(!deserializeJson(doc, payload));
        assert(doc["device"]["identifiers"][0] == "test-bridge_40001");
        if (topic.rfind("homeassistant/event/", 0) == 0) {
            ++event_configs;
            assert(doc["event_types"].size() == 1 && doc["command_topic"].isNull());
            if (doc["name"] == "Button 2") {
                assert(doc["unique_id"] == "test-bridge_40001_button_2");
                assert(doc["availability"].size() == 3);
                assert(doc["event_types"][0] == "activated");
            }
        } else {
            ++trigger_configs;
            assert(doc["automation_type"] == "trigger");
            if (doc["subtype"] == "Button 2")
                assert(doc["type"] == "activated");
        }
    }
    assert(event_configs == 8 && trigger_configs == 7 && button4_offline &&
           button4_trigger_removed);
    messages.clear();
    snapshot.controller = 40003;
    snapshot.product = 126;
    discovery.publish_controller_actions(40003, 126, "Battery Keypad", &snapshot);
    bool battery_button1_ready = false, battery_button4_unassigned = false;
    for (const auto &[topic, payload, retained] : messages) {
        assert(retained);
        if (topic == "test_bridge/controller/40003/action/button_1/availability")
            battery_button1_ready = payload == "online";
        if (topic == "test_bridge/controller/40003/action/button_4/availability")
            battery_button4_unassigned = payload == "offline";
    }
    assert(battery_button1_ready && battery_button4_unassigned);
    messages.clear();
    discovery.publish_controller_actions(40002, 81, "Anywhere", nullptr);
    for (const auto &[topic, payload, retained] : messages)
        assert(topic.find("button_") == std::string::npos);
    assert(messages.size() ==
           10);  // Five event entities and five device triggers, including slider level.
    bool slider_entity = false, slider_trigger = false;
    for (const auto &[topic, payload, retained] : messages) {
        assert(retained);
        if (topic == "homeassistant/event/test-bridge_40002_brightness/config") {
            assert(!deserializeJson(doc, payload));
            assert(doc["name"] == "Brightness" && doc["device_class"].isNull());
            assert(doc["unique_id"] == "test-bridge_40002_brightness");
            assert(doc["state_topic"] == "test_bridge/controller/40002/action/brightness");
            assert(doc["event_types"][0] == "command" && doc["command_topic"].isNull());
            slider_entity = true;
        }
        if (topic == "homeassistant/device_automation/test-bridge_40002_brightness/config")
            slider_trigger = true;
    }
    assert(slider_entity && slider_trigger);
    messages.clear();
    discovery.publish_controller_actions(40004, 91, "Local dimmer", nullptr, true);
    unsigned local_events = 0;
    bool local_brightness = false;
    for (const auto &[topic, payload, retained] : messages) {
        assert(retained && !payload.empty());
        assert(topic.find("button_") == std::string::npos);
        if (topic.rfind("homeassistant/event/", 0) == 0) {
            ++local_events;
            assert(!deserializeJson(doc, payload));
            assert(doc["availability_mode"] == "all" && doc["availability"].size() == 3);
            assert(doc["availability"][2]["topic"] ==
                   "test_bridge/controller/40004/input/availability");
            if (doc["name"] == "Brightness")
                local_brightness = true;
        }
    }
    assert(local_events == 5 && local_brightness && messages.size() == 10);
    messages.clear();
    discovery.publish_controller_actions(40004, 91, "Local dimmer", nullptr, false);
    unsigned cleared = 0;
    for (const auto &[topic, payload, retained] : messages) {
        if (payload.empty()) {
            assert(topic.find("brightness/config") != std::string::npos);
            ++cleared;
        }
    }
    assert(cleared == 2);  // Reversible input mode removes its optional Brightness discovery.
    messages.clear();
    discovery.remove_controller(40001);
    assert(messages.size() == 28);
    for (const auto &[topic, payload, retained] : messages)
        assert(payload.empty() && retained);
}
