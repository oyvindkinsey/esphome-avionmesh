#pragma once
#include "absolute_dimmer.h"
#include "controller_events.h"

namespace avionmesh {
// Observed unassigned model-91 format. This is a level report, including READ
// responses. Transaction-tagged replies are queries; zero-tag reports are
// unrequested inputs unless a matching zero-tag query was recently observed.
inline bool decode_local_dimmer_report(uint8_t product, uint8_t opcode, const uint8_t *payload,
                                       size_t length, ControllerCommand &result) {
    result = {};
    if (product != 91 || opcode != 0x73 || length != 4 || payload[0] != 1 || payload[1] != 0x0a)
        return false;
    result.absolute = true;
    result.brightness = payload[3];
    result.transaction = payload[2];
    result.event_type = payload[3] == 0 ? "off" : payload[3] == 255 ? "on" : "brightness";
    return true;
}

inline bool is_untagged_dimming_query(uint8_t opcode, const uint8_t *payload, size_t length) {
    return opcode == 0x73 && length >= 5 && payload[0] == 1 && payload[1] == 10 && payload[4] == 0;
}

inline bool local_dimmer_report_is_query(uint8_t transaction, bool zero_query_seen,
                                         uint32_t query_ms, uint32_t received_ms) {
    return transaction || (zero_query_seen && static_cast<uint32_t>(received_ms - query_ms) < 2000);
}

class LocalDimmerReports {
 public:
    AbsoluteDimmerDirection::Change observe(uint32_t sequence, uint8_t level, uint32_t received_ms,
                                            bool query_reply = false) {
        sequence &= 0xffffff;
        const uint32_t idle = received_ms - since_;
        const uint32_t advance = (sequence - sequence_) & 0xffffff;
        if (!ready_ || (advance >= 0x800000 && idle > 10000)) {
            seed(sequence, level, received_ms);
            return {};  // Establish/rebase a baseline without firing automations.
        }
        if (!advance || advance >= 0x800000)
            return {};
        const uint8_t previous = level_;
        seed(sequence, level, received_ms);
        if (query_reply)
            return {};  // Refresh a baseline without firing an input.
        const bool direction =
            previous != level && idle <= 15000 && advance <= 128 && previous && level;
        return {direction ? (level > previous ? "raise" : "lower") : nullptr, int(level) - previous,
                previous, true};
    }
    bool ready() const { return ready_; }
    uint8_t level() const { return level_; }

 private:
    void seed(uint32_t sequence, uint8_t level, uint32_t now) {
        ready_ = true;
        sequence_ = sequence;
        level_ = level;
        since_ = now;
    }
    bool ready_{false};
    uint8_t level_{0};
    uint32_t sequence_{0}, since_{0};
};
}  // namespace avionmesh
