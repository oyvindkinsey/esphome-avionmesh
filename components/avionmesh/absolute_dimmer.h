#pragma once
#include <cstdint>

namespace avionmesh {
inline bool supports_absolute_dimmer_direction(uint8_t product) {
    return product == 81 || product == 91;
}
// Direction of an observed level stream, not a physical press/release gesture.
// The source identity and packet freshness are checked by the caller.
class AbsoluteDimmerDirection {
 public:
    struct Change {
        const char *action{nullptr};
        int difference{0};
        uint8_t previous{0};
        bool accepted{false};
    };
    Change observe(uint16_t target, uint32_t sequence, uint8_t level, uint32_t received_ms) {
        sequence &= 0xffffff;
        const uint32_t idle = received_ms - received_ms_;
        if (!initialized_ || target != target_ || idle > 10000) {
            seed(target, sequence, level, received_ms);
            return {nullptr, 0, level, true};
        }
        const uint32_t advance = (sequence - sequence_) & 0xffffff;
        if (!advance || advance >= 0x800000)
            return {};  // Duplicate or reordered mesh packet.
        const uint8_t previous = level_;
        seed(target, sequence, level, received_ms);
        // Never infer direction across an idle gap, a large sequence jump, an
        // Off/On transition, or a stationary level. A subsequent stream sample
        // can establish direction after a fresh baseline.
        if (idle > 1500 || advance > 128 || !previous || !level || level == 255 ||
            level == previous)
            return {nullptr, 0, previous, true};
        return {level > previous ? "raise" : "lower", int(level) - previous, previous, true};
    }

 private:
    void seed(uint16_t target, uint32_t sequence, uint8_t level, uint32_t received_ms) {
        initialized_ = true;
        target_ = target;
        sequence_ = sequence;
        level_ = level;
        received_ms_ = received_ms;
    }
    bool initialized_{false};
    uint16_t target_{0};
    uint32_t sequence_{0}, received_ms_{0};
    uint8_t level_{0};
};
}  // namespace avionmesh
