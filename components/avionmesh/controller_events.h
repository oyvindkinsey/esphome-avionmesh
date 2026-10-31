#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace avionmesh {

inline bool is_controller(uint8_t product) {
    return product == 91 || product == 81 || product == 127 || product == 126;
}

inline const char *controller_name(uint8_t product) {
    switch (product) {
        case 91:
            return "Accessory Dimmer";
        case 81:
            return "Anywhere Dimmer";
        case 127:
            return "Wired Scene Keypad";
        case 126:
            return "Battery Scene Keypad";
        default:
            return "Unknown Controller";
    }
}

struct ControllerCommand {
    const char *event_type{"mesh_command"};
    uint16_t target{0};
    uint8_t brightness{0};
    int delta{0};
    bool absolute{false}, relative{false};
    uint8_t transaction{0};  // Present on local controller READ/status reports.
};

inline bool controller_event_is_fresh(uint32_t received, uint32_t now, uint32_t mqtt_since,
                                      uint32_t mesh_since) {
    return static_cast<uint32_t>(now - received) <= 1000 &&
           static_cast<int32_t>(received - mqtt_since) >= 0 &&
           static_cast<int32_t>(received - mesh_since) >= 0;
}

// Legacy compact-layout decoder. The caller must first authenticate the mesh
// source and establish that it is an inventory controller. New callers should
// use the product-aware overload below; this overload never enables the shifted
// product-91 layout. Commands are not physical button/gesture guesses.
inline bool decode_controller_command(uint8_t opcode, const uint8_t *payload, size_t length,
                                      ControllerCommand &result) {
    result = {};
    if (opcode != 0x73 || length < 2 || (payload[0] != 0 && payload[0] != 0x11))
        return false;  // Read, Ping and assignment traffic are not press events.
    if (length >= 4)
        result.target = (static_cast<uint16_t>(payload[2]) << 8) | payload[3];
    if (length != 10 || payload[1] != 10 || payload[4] || payload[6] || payload[7] || payload[8] ||
        payload[9])
        return true;
    if (payload[0] == 0) {
        result.absolute = true;
        result.brightness = payload[5];
        result.event_type = payload[5] == 0 ? "off" : (payload[5] == 255 ? "on" : "brightness");
    } else if (payload[5] == 0x0d || payload[5] == 0xf3) {
        result.relative = true;
        result.delta = payload[5] == 0x0d ? 13 : -13;
        result.event_type = result.delta > 0 ? "raise" : "lower";
    }
    return true;
}

// authenticated_product must come from an inventory lookup of the decrypted,
// authenticated mesh source, never from payload shape or an advertised identity.
// Pass 0 for an unknown/untrusted source. The decoder cannot authenticate packets.
inline bool decode_controller_command(uint8_t authenticated_product, uint8_t opcode,
                                      const uint8_t *payload, size_t length,
                                      ControllerCommand &result) {
    if (!is_controller(authenticated_product)) {
        result = {};
        return false;
    }
    if (!decode_controller_command(opcode, payload, length, result))
        return false;
    // Additional absolute Write/Dimming layout captured from a native product 91.
    // Byte 5 is an observed zero, not a high level byte. No shifted relative form
    // or other marker value is established by this capture.
    if (authenticated_product == 91 && length == 10 && payload[0] == 0 && payload[1] == 10 &&
        payload[4] == 1 && payload[5] == 0 && payload[7] == 0 && payload[8] == 0 &&
        payload[9] == 0) {
        result.absolute = true;
        result.brightness = payload[6];
        result.event_type = payload[6] == 0 ? "off" : (payload[6] == 255 ? "on" : "brightness");
    }
    return true;
}

// Reassembled encrypted frames have a 40-byte maximum in recsrmesh v1.1.3.
// Copy only here; decrypt, classify, deduplicate and publish on the main loop.
class ControllerPacketQueue {
 public:
    static constexpr size_t CAPACITY = 64, MAX_PACKET = 40;
    struct Packet {
        uint32_t received_ms{0};
        uint8_t length{0};
        std::array<uint8_t, MAX_PACKET> bytes{};
    };
    struct Status {
        size_t queued;
        uint32_t dropped;
    };
    void push(const uint8_t *bytes, size_t length, uint32_t now) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!length || length > MAX_PACKET || count_ == CAPACITY) {
            ++dropped_;
            return;
        }
        auto &packet = packets_[(head_ + count_) % CAPACITY];
        packet.received_ms = now;
        packet.length = length;
        std::memcpy(packet.bytes.data(), bytes, length);
        ++count_;
    }
    bool pop(Packet &packet) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!count_)
            return false;
        packet = packets_[head_];
        head_ = (head_ + 1) % CAPACITY;
        --count_;
        return true;
    }
    // Authenticated snapshots belong to the current mesh key only.
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        head_ = count_ = 0;
    }
    Status status() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {count_, dropped_};
    }
    uint32_t dropped() const { return status().dropped; }

 private:
    mutable std::mutex mutex_;
    std::array<Packet, CAPACITY> packets_{};
    size_t head_{0}, count_{0};
    uint32_t dropped_{0};
};

class ControllerDuplicateWindow {
 public:
    bool accept(const ControllerPacketQueue::Packet &packet, uint32_t now) {
        for (const auto &entry : entries_) {
            if (entry.used && static_cast<uint32_t>(now - entry.seen_ms) < 5000 &&
                entry.packet.length == packet.length &&
                std::memcmp(entry.packet.bytes.data(), packet.bytes.data(), packet.length) == 0)
                return false;
        }
        entries_[next_] = {true, now, packet};
        next_ = (next_ + 1) % entries_.size();
        return true;
    }

 private:
    struct Entry {
        bool used{false};
        uint32_t seen_ms{0};
        ControllerPacketQueue::Packet packet{};
    };
    std::array<Entry, 64> entries_{};
    size_t next_{0};
};

}  // namespace avionmesh
