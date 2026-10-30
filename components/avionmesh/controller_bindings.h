#pragma once

#include "controller_events.h"
#include <algorithm>
#include <array>
#include <string>
#include <set>
#include <vector>

namespace avionmesh {
using ControllerSlotRecords = std::array<std::array<uint8_t, 8>, 4>;
struct ControllerBindingSnapshot {
    uint16_t controller{0};
    uint8_t product{0};
    bool valid{false};
    ControllerSlotRecords records{};
};
inline uint16_t binding_target(const std::array<uint8_t, 8> &r) {
    return static_cast<uint16_t>(r[4]) << 8 | r[5];
}
inline bool valid_binding_records(const ControllerSlotRecords &records) {
    for (unsigned i = 0; i < 4; ++i) {
        const auto &r = records[i];
        if (r[0] != 1 || r[1] != 0x30 || r[2] != 0 || r[3] != i + 1)
            return false;
    }
    return true;
}
inline bool keypad_assignment_model_supported(uint8_t product) {
    return product == 127 || product == 126;
}
// Observed battery Power/Off wake indication. Require a fresh authenticated
// source packet at the call site; cached or ordinary brightness packets do not arm reads.
inline bool battery_keypad_wake_packet(uint8_t opcode, const uint8_t *payload, size_t length,
                                       uint32_t received, uint32_t now) {
    return opcode == 0x73 && payload && length == 7 && payload[0] == 0x0b && payload[1] == 0x1c &&
           static_cast<uint32_t>(now - received) <= 1000;
}
inline const char *binding_slot_status(const ControllerBindingSnapshot *snapshot, uint8_t product,
                                       uint8_t slot) {
    if (!keypad_assignment_model_supported(product))
        return "model_unverified";
    if (!snapshot || !snapshot->valid || snapshot->product != product || slot < 1 || slot > 4)
        return "needs_refresh";
    const auto &r = snapshot->records[slot - 1];
    const auto target = binding_target(r);
    if (target == 65535 && r[6] == 0 && r[7] == 0)
        return "unassigned";
    if (!target || target == 65535 || r[6] != 8 || !r[7])
        return "unsupported_binding";
    for (unsigned i = 0; i < 4; ++i)
        if (i != slot - 1u && binding_target(snapshot->records[i]) == target)
            return "ambiguous_target";
    return "ready";
}
inline uint8_t binding_context_slot(const ControllerBindingSnapshot *snapshot, uint8_t product,
                                    uint16_t target) {
    for (uint8_t slot = 1; slot <= 4; ++slot)
        if (std::string(binding_slot_status(snapshot, product, slot)) == "ready" &&
            binding_target(snapshot->records[slot - 1]) == target)
            return slot;
    return 0;
}
inline uint8_t binding_activation_slot(const ControllerBindingSnapshot *snapshot, uint8_t product,
                                       uint16_t target, const ControllerCommand &command) {
    // Relative dimming and OFF carry context, not evidence of a scene-key press.
    if (!command.absolute || !command.brightness)
        return 0;
    auto slot = binding_context_slot(snapshot, product, target);
    return slot && snapshot->records[slot - 1][7] == command.brightness ? slot : 0;
}

// Stable key fingerprint prevents a saved mapping crossing into a different mesh.
inline uint64_t controller_mesh_tag(const std::string &key) {
    uint64_t tag = 14695981039346656037ULL;
    for (unsigned char c : key) {
        tag ^= c;
        tag *= 1099511628211ULL;
    }
    return tag;
}
class ControllerBindings {
 public:
    static constexpr size_t MAX_CONTROLLERS = 64;
    void load(const std::string &mesh_key);
    bool bind_mesh(const std::string &mesh_key);
    bool set(uint16_t controller, uint8_t product, const ControllerSlotRecords &records);
    bool invalidate(uint16_t controller);
    void invalidate_all();
    void remove(uint16_t controller);
    const ControllerBindingSnapshot *find(uint16_t controller) const {
        for (const auto &s : snapshots_)
            if (s.controller == controller)
                return &s;
        return nullptr;
    }
    std::set<uint16_t> known_targets() const {
        std::set<uint16_t> targets;
        for (const auto &s : snapshots_)
            for (const auto &r : s.records)
                targets.insert(binding_target(r));
        return targets;
    }
    std::vector<uint8_t> encode() const;
    bool decode(const std::vector<uint8_t> &blob, const std::string &mesh_key);
    size_t size() const { return snapshots_.size(); }
    size_t capacity() const { return snapshots_.capacity(); }
    size_t valid_count() const {
        return static_cast<size_t>(
            std::count_if(snapshots_.begin(), snapshots_.end(),
                          [](const auto &snapshot) { return snapshot.valid; }));
    }

 private:
    uint64_t mesh_tag_{0};
    std::vector<ControllerBindingSnapshot> snapshots_;
    bool save() const;
};
}  // namespace avionmesh
