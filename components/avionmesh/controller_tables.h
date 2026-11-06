#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace avionmesh {

// Diagnostic access to known table nouns only. The payload is COUNT or READ;
// no caller-supplied bytes, writes, inserts or deletes can pass through here.
inline bool build_controller_probe(uint32_t target, uint32_t noun, bool count, uint32_t slot,
                                   uint8_t *payload, size_t &length, bool padded = false,
                                   uint8_t transaction = 0) {
    if (target < 32896 || target > 65407 || slot > 20 || (count && slot != 0) ||
        (noun != 3 && noun != 27 && noun != 34 && noun != 48 && noun != 10) ||
        (noun == 10 && (count || slot != 0)))
        return false;
    length = padded ? 10 : count ? 7 : 8;
    for (size_t i = 0; i < length; ++i)
        payload[i] = 0;
    payload[0] = count ? 4 : 1;
    payload[1] = noun;
    payload[4] = transaction;
    if (!count)
        payload[5] = slot;
    return true;
}

inline bool controller_probe_reply_matches(uint16_t target, uint8_t noun, bool count,
                                           uint16_t source, uint8_t opcode, const uint8_t *payload,
                                           size_t length) {
    return source == target && opcode == 0x73 && length >= 2 && payload[0] == (count ? 4 : 1) &&
           payload[1] == noun;
    // Deliberately does not interpret record indexes/counts or prove emptiness.
}

// The wire table has four slots. Additional guards below protect occupied
// native bindings when setting up previously unused slots 1 and 4.
inline bool controller_program_slot_supported(uint32_t slot) {
    return slot >= 1 && slot <= 4;
}

inline std::string controller_unused_record(uint8_t slot) {
    if (slot < 1 || slot > 4)
        return {};
    return "0130000" + std::to_string(slot) + "ffff0000";
}

inline bool controller_extra_slot_write_allowed(uint32_t slot, bool clear,
                                                const std::string &expected, bool reserved_target,
                                                bool currently_owned_target) {
    if (!controller_program_slot_supported(slot))
        return false;
    if (slot == 2 || slot == 3)
        return true;
    // Never replace or delete an occupied native binding in these slots.
    return clear ? currently_owned_target
                 : reserved_target &&
                       expected == controller_unused_record(static_cast<uint8_t>(slot));
}

// Observed ASSOCIATION_2 COUNT/READ format. Slot 0 means COUNT; 1..4 READ.
// This deliberately provides no guessed assignment-write encoding.
inline bool build_controller_table_read(uint16_t target, uint8_t slot, uint8_t *payload,
                                        size_t &length) {
    if (target < 32896 || target > 65407 || slot > 4)
        return false;
    length = slot ? 8 : 7;
    for (size_t i = 0; i < length; ++i)
        payload[i] = 0;
    payload[0] = slot ? 1 : 4;
    payload[1] = 0x30;
    if (slot)
        payload[5] = slot;
    return true;
}

inline bool controller_table_reply_matches(uint16_t expected_source, uint8_t slot, uint16_t source,
                                           uint8_t opcode, const uint8_t *payload, size_t length) {
    if (expected_source != source || opcode != 0x73 || slot > 4)
        return false;
    if (length != (slot ? 8u : 5u))
        return false;
    return payload[0] == (slot ? 1 : 4) && payload[1] == 0x30 && payload[2] == 0 &&
           (!slot || payload[3] == slot);
}

inline std::string controller_event_target_name(uint16_t controller, uint8_t slot) {
    return "HA event target: " + std::to_string(controller) + ":" + std::to_string(slot);
}

inline bool is_controller_event_target(const std::string &name) {
    return name.rfind("HA event target: ", 0) == 0;
}

inline bool controller_snapshot_matches(const std::string &actual, const std::string &expected,
                                        uint32_t seen, uint32_t now) {
    return expected.size() == 16 && actual == expected && static_cast<uint32_t>(now - seen) < 15000;
}

// Typed forms from vendor-app traffic. A reply code is not proof of persistence;
// the host must reconcile by fresh READs and never retry an uncertain write.
inline bool build_controller_table_program(uint16_t controller, uint8_t slot,
                                           uint8_t transaction_id, bool clear, uint16_t group,
                                           uint8_t *payload) {
    if (controller < 32896 || controller > 65407 || slot < 1 || slot > 4 ||
        (!clear && (group < 256 || group > 24575)))
        return false;
    for (size_t i = 0; i < 10; ++i)
        payload[i] = 0;
    payload[0] = clear ? 5 : 2;
    payload[1] = 0x30;
    payload[4] = transaction_id;
    payload[5] = slot;
    if (!clear) {
        payload[6] = group >> 8;
        payload[7] = group & 255;
        payload[8] = 8;
        payload[9] = 255;
    }
    return true;
}

inline bool controller_program_reply_matches(uint16_t target, uint8_t transaction_id, bool clear,
                                             uint16_t source, uint8_t opcode,
                                             const uint8_t *payload, size_t length) {
    return length == 4 && source == target && opcode == 0x73 && payload[0] == (clear ? 5 : 2) &&
           payload[1] == 0x30 && payload[2] == transaction_id;
}

// Observed second INSERT in the same vendor-app edit: slot | 0x20 and
// zero target/flags/value. Its exact semantics remain a test hypothesis until
// primary + secondary packets produce a verified assignment READback.
inline bool build_controller_table_secondary(uint16_t controller, uint8_t slot,
                                             uint8_t transaction_id, uint8_t *payload) {
    if (controller < 32896 || controller > 65407 || slot < 1 || slot > 4)
        return false;
    for (size_t i = 0; i < 10; ++i)
        payload[i] = 0;
    payload[0] = 2;
    payload[1] = 0x30;
    payload[4] = transaction_id;
    payload[5] = slot | 0x20;
    return true;
}

}  // namespace avionmesh
