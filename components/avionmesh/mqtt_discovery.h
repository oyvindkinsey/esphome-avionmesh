#pragma once

#include <cstdint>
#include <string>

namespace avionmesh {

class MqttDiscovery {
 public:
    void set_node_name(const std::string &name) { node_name_ = name; }
    void set_topic_prefix(const std::string &prefix) { topic_prefix_ = prefix; }

    void publish_light(uint16_t avion_id, const std::string &name,
                       bool has_brightness, bool has_color_temp,
                       const std::string &product_name = "");

    void remove_light(uint16_t avion_id);

    void publish_on_off_state(uint16_t avion_id, bool on);
    void publish_brightness_state(uint16_t avion_id, uint8_t brightness);
    void publish_color_temp_state(uint16_t avion_id, uint16_t kelvin);

    std::string state_topic(uint16_t avion_id) const;
    std::string command_topic(uint16_t avion_id) const;
    std::string brightness_state_topic(uint16_t avion_id) const;
    std::string brightness_command_topic(uint16_t avion_id) const;
    std::string color_temp_state_topic(uint16_t avion_id) const;
    std::string color_temp_command_topic(uint16_t avion_id) const;
    std::string discovery_topic(uint16_t avion_id) const;
    std::string management_command_topic() const;
    std::string management_response_topic() const;

 protected:
    std::string node_name_;
    std::string topic_prefix_;

    void publish_(const std::string &topic, const std::string &payload, bool retain = false);
};

}  // namespace avionmesh
