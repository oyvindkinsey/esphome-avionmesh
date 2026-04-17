#include "mqtt_discovery.h"

#ifdef USE_ESP32
#include "esphome/components/mqtt/mqtt_client.h"
#endif

namespace avionmesh {

std::string MqttDiscovery::state_topic(uint16_t avion_id) const {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s/light/%u/state",
             topic_prefix_.c_str(), avion_id);
    return buf;
}

std::string MqttDiscovery::command_topic(uint16_t avion_id) const {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s/light/%u/set",
             topic_prefix_.c_str(), avion_id);
    return buf;
}

std::string MqttDiscovery::discovery_topic(uint16_t avion_id) const {
    char buf[128];
    snprintf(buf, sizeof(buf), "homeassistant/light/%s_%u/config",
             node_name_.c_str(), avion_id);
    return buf;
}

std::string MqttDiscovery::management_command_topic() const {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s/avionmesh/command",
             topic_prefix_.c_str());
    return buf;
}

std::string MqttDiscovery::management_response_topic() const {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s/avionmesh/response",
             topic_prefix_.c_str());
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

void MqttDiscovery::publish_light(uint16_t avion_id, const std::string &name,
                                   bool has_brightness, bool has_color_temp,
                                   const std::string &product_name) {
    char uid[64];
    snprintf(uid, sizeof(uid), "%s_%u", node_name_.c_str(), avion_id);

    std::string config = "{";
    config += "\"name\":\"" + name + "\",";
    config += "\"unique_id\":\"" + std::string(uid) + "\",";
    config += "\"schema\":\"json\",";
    config += "\"command_topic\":\"" + command_topic(avion_id) + "\",";
    config += "\"state_topic\":\"" + state_topic(avion_id) + "\",";

    if (has_brightness) {
        config += "\"brightness\":true,";
        config += "\"brightness_scale\":255,";
    }
    if (has_color_temp) {
        config += "\"supported_color_modes\":[\"color_temp\"],";
        config += "\"min_mireds\":200,";
        config += "\"max_mireds\":370,";
    } else if (has_brightness) {
        config += "\"supported_color_modes\":[\"brightness\"],";
    }

    config += "\"device\":{";
    config += "\"identifiers\":[\"" + std::string(uid) + "\"],";
    config += "\"name\":\"" + name + "\",";
    config += "\"manufacturer\":\"Avi-on\",";
    if (!product_name.empty())
        config += "\"model\":\"" + product_name + "\",";
    config += "\"via_device\":\"" + node_name_ + "\"";
    config += "}";

    config += "}";

    publish_(discovery_topic(avion_id), config, true);
}

void MqttDiscovery::remove_light(uint16_t avion_id) {
    publish_(discovery_topic(avion_id), "", true);
}

void MqttDiscovery::publish_state(uint16_t avion_id, bool on, uint8_t brightness,
                                   bool has_color_temp, bool color_temp_known,
                                   uint16_t color_temp_kelvin) {
    char buf[128];
    if (has_color_temp && color_temp_known) {
        uint16_t mireds = color_temp_kelvin > 0 ? 1000000u / color_temp_kelvin : 0;
        snprintf(buf, sizeof(buf),
                 "{\"state\":\"%s\",\"brightness\":%u,\"color_mode\":\"color_temp\",\"color_temp\":%u}",
                 on ? "ON" : "OFF", brightness, mireds);
    } else {
        snprintf(buf, sizeof(buf),
                 "{\"state\":\"%s\",\"brightness\":%u}",
                 on ? "ON" : "OFF", brightness);
    }
    publish_(state_topic(avion_id), buf, true);
}

}  // namespace avionmesh
