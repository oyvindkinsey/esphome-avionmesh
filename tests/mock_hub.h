#pragma once

#include "../components/avionmesh/avionmesh_hub.h"

#include <initializer_list>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace avionmesh {

class TestHub : public AvionMeshHub {
 public:
    std::vector<Command> mesh_sends;
    std::vector<std::tuple<std::string, std::string, bool>> mqtt_publishes;
    std::map<std::string, std::function<void(const std::string &, const std::string &)>> mqtt_subs;
    std::vector<std::pair<std::string, std::string>> sse_events;

    // Call after populating db_ and setting mqtt_exposed flags.
    // Wires discovery_ publish function and subscribes all MQTT command topics.
    void test_setup() {
        discovery_.set_node_name("test");
        discovery_.set_topic_prefix("avionmesh");
        discovery_.set_publish_fn([this](const std::string &t, const std::string &p, bool r) {
            do_mqtt_publish(t, p, r);
        });
        subscribe_all_commands();
        clear_captures();
    }

    // Simulate a brightness status arriving over BLE.
    // Verb=Write(0), Noun=Dimming(0x0A), value_bytes[1]=brightness
    void inject_brightness(uint16_t device_id, uint8_t brightness) {
        uint8_t payload[] = {0x00, 0x0A, 0x00, 0x00, 0x00, brightness, 0x00, 0x00, 0x00, 0x00};
        on_mesh_rx(device_id, device_id, 0x73, payload, sizeof(payload));
    }

    // Simulate a color-temp status arriving over BLE.
    // Verb=Write(0), Noun=Color(0x1D), value_bytes[2..3]=kelvin big-endian
    void inject_color_temp(uint16_t device_id, uint16_t kelvin) {
        uint8_t payload[] = {
            0x00,
            0x1D,
            0x00,
            0x00,
            0x00,
            0x00,
            static_cast<uint8_t>(kelvin >> 8),
            static_cast<uint8_t>(kelvin & 0xFF),
            0x00,
            0x00,
            0x00,
        };
        on_mesh_rx(device_id, device_id, 0x73, payload, sizeof(payload));
    }

    // Deliver an MQTT message to the matching subscribed callback.
    void inject_mqtt(const std::string &topic, const std::string &payload) {
        auto it = mqtt_subs.find(topic);
        if (it != mqtt_subs.end())
            it->second(topic, payload);
    }

    // Push a deferred action and drain it immediately (simulating loop()).
    void push_action(DeferredAction act) {
        {
            std::lock_guard<std::mutex> lock(action_mutex_);
            pending_actions_.push_back(std::move(act));
        }
        process_deferred_actions();
    }

    // Simulate a ping reply arriving during mesh discovery/scan.
    // MODEL_OPCODE=0x73, payload[0]=Verb::Ping(6), then raw reply bytes.
    void inject_ping(uint16_t device_id, std::initializer_list<uint8_t> reply) {
        std::vector<uint8_t> payload;
        payload.push_back(0x06);
        for (uint8_t b : reply)
            payload.push_back(b);
        on_mesh_rx(device_id, device_id, 0x73, payload.data(), payload.size());
    }

    // Deliver a payload buffer with an explicit length (may be shorter than
    // the backing buffer to simulate short replies with sentinel bytes past len).
    void inject_ping_raw(uint16_t device_id, const uint8_t *data, size_t len) {
        on_mesh_rx(device_id, device_id, 0x73, data, len);
    }

    void set_discovering(bool on) { discovering_mesh_ = on; }

    const std::vector<DiscoveredDevice> &discovered() const { return discovered_devices_; }

    DeviceDB &db() { return db_; }
    std::map<uint16_t, DeviceState> &states() { return device_states_; }

    void clear_captures() {
        mesh_sends.clear();
        mqtt_publishes.clear();
        sse_events.clear();
    }

 protected:
    void do_mesh_send(const Command &cmd) override { mesh_sends.push_back(cmd); }

    void do_mqtt_publish(const std::string &topic, const std::string &payload,
                         bool retain) override {
        mqtt_publishes.emplace_back(topic, payload, retain);
    }

    void do_mqtt_subscribe(
        const std::string &topic,
        std::function<void(const std::string &, const std::string &)> cb) override {
        mqtt_subs[topic] = std::move(cb);
    }

    void do_sse_emit(const std::string &event, const std::string &data) override {
        sse_events.emplace_back(event, data);
    }
};

}  // namespace avionmesh
