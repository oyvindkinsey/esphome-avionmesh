#include "mqtt_discovery.h"
#include "esphome/components/json/json_util.h"

#ifdef USE_ESP32
#include "esphome/components/mqtt/mqtt_client.h"
#endif

namespace avionmesh {

std::string MqttDiscovery::state_topic(uint16_t avion_id) const {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s/light/%u/state", topic_prefix_.c_str(), avion_id);
    return buf;
}

std::string MqttDiscovery::command_topic(uint16_t avion_id) const {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s/light/%u/set", topic_prefix_.c_str(), avion_id);
    return buf;
}

std::string MqttDiscovery::brightness_state_topic(uint16_t avion_id) const {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s/light/%u/brightness/state", topic_prefix_.c_str(), avion_id);
    return buf;
}

std::string MqttDiscovery::brightness_command_topic(uint16_t avion_id) const {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s/light/%u/brightness/set", topic_prefix_.c_str(), avion_id);
    return buf;
}

std::string MqttDiscovery::color_temp_state_topic(uint16_t avion_id) const {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s/light/%u/color_temp/state", topic_prefix_.c_str(), avion_id);
    return buf;
}

std::string MqttDiscovery::color_temp_command_topic(uint16_t avion_id) const {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s/light/%u/color_temp/set", topic_prefix_.c_str(), avion_id);
    return buf;
}

std::string MqttDiscovery::discovery_topic(uint16_t avion_id) const {
    char buf[128];
    snprintf(buf, sizeof(buf), "homeassistant/light/%s_%u/config", node_name_.c_str(), avion_id);
    return buf;
}

std::string MqttDiscovery::management_command_topic() const {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s/avionmesh/command", topic_prefix_.c_str());
    return buf;
}

std::string MqttDiscovery::management_response_topic() const {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s/avionmesh/response", topic_prefix_.c_str());
    return buf;
}

void MqttDiscovery::publish_(const std::string &topic, const std::string &payload, bool retain) {
    if (publish_fn_) {
        publish_fn_(topic, payload, retain);
        return;
    }
#ifdef USE_ESP32
    auto *mqtt = esphome::mqtt::global_mqtt_client;
    if (mqtt)
        mqtt->publish(topic, payload, 0, retain);
#endif
}

void MqttDiscovery::publish_light(uint16_t avion_id, const std::string &name, bool has_brightness,
                                  bool has_color_temp, const std::string &product_name) {
    char uid[64];
    snprintf(uid, sizeof(uid), "%s_%u", node_name_.c_str(), avion_id);

    std::string object_id;
    for (char c : name) {
        if (c == ' ')
            object_id += '_';
        else if (c >= 'A' && c <= 'Z')
            object_id += static_cast<char>(c + 32);
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
            object_id += c;
    }

    if (object_id.empty())
        object_id = uid;

    std::string config = "{";
    config += "\"name\":\"" + name + "\",";
    config += "\"unique_id\":\"" + std::string(uid) + "\",";
    config += "\"object_id\":\"" + object_id + "\",";
    config += "\"command_topic\":\"" + command_topic(avion_id) + "\",";
    config += "\"state_topic\":\"" + state_topic(avion_id) + "\",";

    if (has_brightness) {
        config += "\"brightness_command_topic\":\"" + brightness_command_topic(avion_id) + "\",";
        config += "\"brightness_state_topic\":\"" + brightness_state_topic(avion_id) + "\",";
        config += "\"brightness_scale\":255,";
    }
    if (has_color_temp) {
        config += "\"supported_color_modes\":[\"color_temp\"],";
        config += "\"min_mireds\":200,";
        config += "\"max_mireds\":370,";
        config += "\"color_temp_command_topic\":\"" + color_temp_command_topic(avion_id) + "\",";
        config += "\"color_temp_state_topic\":\"" + color_temp_state_topic(avion_id) + "\",";
    } else if (has_brightness) {
        config += "\"supported_color_modes\":[\"brightness\"],";
    }

    config += "\"device\":{";
    if (single_device_) {
        config += "\"identifiers\":[\"" + node_name_ + "\"],";
        config += "\"name\":\"Avion Mesh Bridge\",";
        config += "\"manufacturer\":\"Avi-on\"";
    } else {
        config += "\"identifiers\":[\"" + std::string(uid) + "\"],";
        config += "\"name\":\"" + name + "\",";
        config += "\"manufacturer\":\"Avi-on\",";
        if (!product_name.empty())
            config += "\"model\":\"" + product_name + "\",";
        config += "\"via_device\":\"" + node_name_ + "\"";
    }
    config += "}";

    config += "}";

    publish_(discovery_topic(avion_id), config, true);
}

void MqttDiscovery::remove_light(uint16_t avion_id) {
    publish_(discovery_topic(avion_id), "", true);
}

std::string MqttDiscovery::controller_event_topic(uint16_t avion_id) const {
    return topic_prefix_ + "/controller/" + std::to_string(avion_id) + "/event";
}

std::string MqttDiscovery::controller_discovery_topic(uint16_t avion_id) const {
    return "homeassistant/event/" + node_name_ + "_" + std::to_string(avion_id) + "/config";
}

std::string MqttDiscovery::controller_availability_topic() const {
    return topic_prefix_ + "/controller/availability";
}

void MqttDiscovery::publish_controller(uint16_t avion_id, const std::string &name,
                                       const std::string &model) {
    auto identity = node_name_ + "_" + std::to_string(avion_id);
    auto config = esphome::json::build_json([&](JsonObject root) {
        root["name"] = "Commands";
        root["unique_id"] = identity + "_commands";
        root["state_topic"] = controller_event_topic(avion_id);
        root["device_class"] = "button";
        root["icon"] = "mdi:gesture-tap-button";
        auto types = root["event_types"].to<JsonArray>();
        for (const char *type : {"on", "off", "brightness", "raise", "lower", "mesh_command"})
            types.add(type);
        root["availability_mode"] = "all";
        auto availability = root["availability"].to<JsonArray>();
        availability.add<JsonObject>()["topic"] = topic_prefix_ + "/status";
        availability.add<JsonObject>()["topic"] = controller_availability_topic();
        auto device = root["device"].to<JsonObject>();
        device["identifiers"].to<JsonArray>().add(identity);
        device["name"] = name;
        device["manufacturer"] = "Avi-on / HALO Home";
        device["model"] = model;
        device["via_device"] = node_name_;
    });
    publish_(controller_discovery_topic(avion_id), config, true);
}

void MqttDiscovery::remove_controller(uint16_t avion_id) {
    publish_(controller_discovery_topic(avion_id), "", true);
    publish_(controller_input_availability_topic(avion_id), "", true);
    const auto identity = node_name_ + "_" + std::to_string(avion_id);
    for (const char *action : {"button_1", "button_2", "button_3", "button_4", "raise", "lower",
                               "on", "off", "brightness"}) {
        publish_("homeassistant/event/" + identity + "_" + action + "/config", "", true);
        publish_("homeassistant/device_automation/" + identity + "_" + action + "/config", "",
                 true);
        if (std::string(action).rfind("button_", 0) == 0) {
            const auto topic = controller_action_topic(avion_id, action);
            publish_(topic + "/availability", "", true);
            publish_(topic + "/mapping", "", true);
        }
    }
}

std::string MqttDiscovery::controller_action_topic(uint16_t avion_id,
                                                   const std::string &action) const {
    return topic_prefix_ + "/controller/" + std::to_string(avion_id) + "/action/" + action;
}

std::string MqttDiscovery::controller_input_availability_topic(uint16_t avion_id) const {
    return topic_prefix_ + "/controller/" + std::to_string(avion_id) + "/input/availability";
}

void MqttDiscovery::publish_controller_actions(uint16_t avion_id, uint8_t product,
                                               const std::string &name,
                                               const ControllerBindingSnapshot *snapshot,
                                               bool local_level) {
    const auto identity = node_name_ + "_" + std::to_string(avion_id);
    for (unsigned i = 0; i < 9; ++i) {
        const bool button = i < 4;
        const bool brightness = i == 8;
        if (button && product != 127 && product != 126)
            continue;
        if (brightness && product != 81 && !(product == 91 && local_level)) {
            // Remove this optional entity/trigger when an accessory switches
            // back to native commands. Other controller layouts stay unchanged.
            if (product == 91) {
                publish_("homeassistant/event/" + identity + "_brightness/config", "", true);
                publish_("homeassistant/device_automation/" + identity + "_brightness/config", "",
                         true);
            }
            continue;
        }
        const uint8_t slot = i + 1;
        const std::string key = brightness ? "brightness"
                                : button   ? "button_" + std::to_string(slot)
                                           : (i == 4   ? "raise"
                                              : i == 5 ? "lower"
                                              : i == 6 ? "on"
                                                       : "off");
        const std::string label = brightness ? "Brightness"
                                  : button   ? "Button " + std::to_string(slot)
                                             : (i == 4   ? "Raise"
                                                : i == 5 ? "Lower"
                                                : i == 6 ? "On"
                                                         : "Off");
        const auto topic = controller_action_topic(avion_id, key);
        const char *event_type = button ? "activated" : "command";
        const auto status = button ? binding_slot_status(snapshot, product, slot) : "ready";
        const bool ready = std::string(status) == "ready";
        auto device_info = [&](JsonObject device) {
            device["identifiers"].to<JsonArray>().add(identity);
            device["name"] = name;
            device["manufacturer"] = "Avi-on / HALO Home";
            device["model"] = controller_name(product);
            device["via_device"] = node_name_;
        };
        auto event_config = esphome::json::build_json([&](JsonObject root) {
            root["name"] = label;
            root["unique_id"] = identity + "_" + key;
            root["state_topic"] = topic;
            if (brightness)
                root["icon"] = "mdi:tune-vertical";
            else
                root["device_class"] = "button";
            root["event_types"].to<JsonArray>().add(event_type);
            root["availability_mode"] = "all";
            auto availability = root["availability"].to<JsonArray>();
            availability.add<JsonObject>()["topic"] = topic_prefix_ + "/status";
            availability.add<JsonObject>()["topic"] = controller_availability_topic();
            if (product == 91 && local_level)
                availability.add<JsonObject>()["topic"] =
                    controller_input_availability_topic(avion_id);
            if (button) {
                availability.add<JsonObject>()["topic"] = topic + "/availability";
                root["json_attributes_topic"] = topic + "/mapping";
            }
            device_info(root["device"].to<JsonObject>());
        });
        publish_("homeassistant/event/" + identity + "_" + key + "/config", event_config, true);
        const auto trigger_topic =
            "homeassistant/device_automation/" + identity + "_" + key + "/config";
        if (ready) {
            auto trigger_config = esphome::json::build_json([&](JsonObject root) {
                root["automation_type"] = "trigger";
                root["type"] = button ? "activated" : "command";
                root["subtype"] = label;
                root["topic"] = topic;
                root["value_template"] = "{{ value_json.event_type }}";
                root["payload"] = event_type;
                device_info(root["device"].to<JsonObject>());
            });
            publish_(trigger_topic, trigger_config, true);
        } else
            publish_(trigger_topic, "", true);
        if (button) {
            publish_(topic + "/availability", ready ? "online" : "offline", true);
            const auto mapping = esphome::json::build_json([&](JsonObject root) {
                root["mapping_status"] = status;
                if (snapshot && snapshot->valid && snapshot->product == product) {
                    root["target_id"] = binding_target(snapshot->records[slot - 1]);
                    root["native_brightness"] = snapshot->records[slot - 1][7];
                }
            });
            publish_(topic + "/mapping", mapping, true);
        }
    }
}

void MqttDiscovery::publish_on_off_state(uint16_t avion_id, bool on) {
    publish_(state_topic(avion_id), on ? "ON" : "OFF", true);
}

void MqttDiscovery::publish_brightness_state(uint16_t avion_id, uint8_t brightness) {
    char payload[8];
    snprintf(payload, sizeof(payload), "%u", brightness);
    publish_(brightness_state_topic(avion_id), payload, true);
}

void MqttDiscovery::publish_color_temp_state(uint16_t avion_id, uint16_t kelvin) {
    char payload[8];
    uint16_t mireds = kelvin > 0 ? 1000000u / kelvin : 0;
    snprintf(payload, sizeof(payload), "%u", mireds);
    publish_(color_temp_state_topic(avion_id), payload, true);
}

}  // namespace avionmesh
