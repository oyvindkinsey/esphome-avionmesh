#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace avionmesh {

class MqttDiscovery {
 public:
    void set_node_name(const std::string &name) { node_name_ = name; }
    void set_topic_prefix(const std::string &prefix) { topic_prefix_ = prefix; }
    void set_publish_fn(std::function<void(const std::string &, const std::string &, bool)> fn) {
        publish_fn_ = std::move(fn);
    }

    void publish_light(uint16_t avion_id, const std::string &name,
                       bool has_brightness, bool has_color_temp,
                       const std::string &product_name = "");

    void remove_light(uint16_t avion_id);

    void publish_state(uint16_t avion_id, bool on, uint8_t brightness,
                       bool has_color_temp, bool color_temp_known,
                       uint16_t color_temp_kelvin);

    std::string state_topic(uint16_t avion_id) const;
    std::string command_topic(uint16_t avion_id) const;
    std::string discovery_topic(uint16_t avion_id) const;
    std::string management_command_topic() const;
    std::string management_response_topic() const;

 protected:
    std::string node_name_;
    std::string topic_prefix_;
    std::function<void(const std::string &, const std::string &, bool)> publish_fn_;

    void publish_(const std::string &topic, const std::string &payload, bool retain = false);
};

}  // namespace avionmesh
