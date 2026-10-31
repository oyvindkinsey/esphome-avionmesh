#include "controller_bindings.h"
#include <cassert>
#include <cstring>
using namespace avionmesh;

int main() {
    const ControllerSlotRecords records{{{{1, 0x30, 0, 1, 0x04, 0xb0, 8, 255}},
                                         {{1, 0x30, 0, 2, 0x5f, 0xfe, 8, 255}},
                                         {{1, 0x30, 0, 3, 0x5f, 0xfd, 8, 255}},
                                         {{1, 0x30, 0, 4, 0xff, 0xff, 0, 0}}}};
    ControllerBindings bindings;
    bindings.load("test-mesh-key");
    assert(bindings.set(40101, 127, records));
    auto *snapshot = bindings.find(40101);
    assert(snapshot && snapshot->valid);
    assert(!std::strcmp(binding_slot_status(snapshot, 127, 1), "ready"));
    assert(!std::strcmp(binding_slot_status(snapshot, 127, 4), "unassigned"));
    assert(
        !std::strcmp(binding_slot_status(snapshot, 126, 1), "needs_refresh"));  // Product mismatch.
    assert(!std::strcmp(binding_slot_status(nullptr, 127, 1), "needs_refresh"));
    ControllerCommand command;
    command.absolute = true;
    command.brightness = 255;
    assert(binding_activation_slot(snapshot, 127, 1200, command) == 1);
    assert(binding_activation_slot(snapshot, 127, 24574, command) == 2);
    assert(binding_activation_slot(snapshot, 127, 24573, command) == 3);
    assert(binding_activation_slot(snapshot, 127, 999, command) == 0);
    command.brightness = 128;
    assert(binding_activation_slot(snapshot, 127, 1200, command) == 0);
    command.brightness = 0;
    assert(binding_activation_slot(snapshot, 127, 24574, command) == 0);
    command.absolute = false;
    command.relative = true;
    command.delta = 13;
    assert(binding_activation_slot(snapshot, 127, 24574, command) == 0);
    assert(binding_context_slot(snapshot, 127, 24574) == 2);
    // Two buttons with the same target cannot be separated even if levels differ.
    auto ambiguous = records;
    ambiguous[1][4] = 4;
    ambiguous[1][5] = 0xb0;
    ambiguous[1][7] = 128;
    assert(bindings.set(40001, 127, ambiguous));
    assert(!std::strcmp(binding_slot_status(bindings.find(40001), 127, 1), "ambiguous_target"));
    assert(!std::strcmp(binding_slot_status(bindings.find(40001), 127, 2), "ambiguous_target"));
    // A different keypad uses the same protocol with independent assignments.
    auto second = records;
    second[0][4] = 4;
    second[0][5] = 0xb1;
    assert(bindings.set(40002, 127, second));
    assert(binding_context_slot(bindings.find(40002), 127, 1201) == 1);
    assert(binding_context_slot(bindings.find(40101), 127, 1201) == 0);
    auto unknown = records;
    unknown[0][6] = 32;
    assert(bindings.set(40003, 127, unknown));
    assert(!std::strcmp(binding_slot_status(bindings.find(40003), 127, 1), "unsupported_binding"));
    auto blob = bindings.encode();
    ControllerBindings restored;
    assert(restored.decode(blob, "test-mesh-key"));
    assert(binding_context_slot(restored.find(40101), 127, 24573) == 3);
    assert(!restored.decode(blob, "different-mesh"));
    assert(!restored.find(40101));
    assert(restored.decode(blob, "test-mesh-key"));
    assert(restored.invalidate(40101));
    assert(!std::strcmp(binding_slot_status(restored.find(40101), 127, 1), "needs_refresh"));
    assert(restored.decode(restored.encode(), "test-mesh-key"));
    assert(!restored.find(40101)->valid);
    assert(restored.bind_mesh("new-key"));
    assert(!restored.find(40001));
    auto truncated = blob;
    truncated.pop_back();
    assert(!restored.decode(truncated, "test-mesh-key"));
    auto malformed = records;
    malformed[1][3] = 1;
    assert(!bindings.set(40004, 127, malformed));
    assert(bindings.set(40004, 126, records));
    assert(!std::strcmp(binding_slot_status(bindings.find(40004), 126, 1), "ready"));
    command.absolute = true;
    command.relative = false;
    command.brightness = 255;
    assert(binding_activation_slot(bindings.find(40004), 126, 1200, command) == 1);
    assert(binding_activation_slot(bindings.find(40004), 126, 24574, command) == 2);
    assert(!std::strcmp(binding_slot_status(bindings.find(40004), 91, 1), "model_unverified"));
    assert(keypad_assignment_model_supported(126));
    assert(!keypad_assignment_write_supported(126));
    assert(keypad_assignment_write_supported(127));
    assert(!std::strcmp(keypad_programming_status(126), "programming_unverified"));
    const uint8_t wake[] = {0x0b, 0x1c, 1, 0x0b, 5, 100, 1};
    assert(battery_keypad_wake_packet(0x73, wake, sizeof(wake), 1000, 1100));
    assert(!battery_keypad_wake_packet(0x73, wake, sizeof(wake), 1000, 2001));
    assert(!battery_keypad_wake_packet(0x73, wake, 2, 1000, 1100));
    assert(!battery_keypad_wake_packet(0, wake, sizeof(wake), 1000, 1100));
    assert(!battery_keypad_wake_packet(0x73, nullptr, 7, 1000, 1100));
    for (unsigned i = 0; i < sizeof(wake); ++i) {
        auto changed = std::array<uint8_t, 7>{};
        std::copy_n(wake, 7, changed.begin());
        changed[i] ^= 1;
        assert(battery_keypad_wake_packet(0x73, changed.data(), 7, 1000, 1100) == (i >= 2));
    }
    assert(battery_keypad_wake_packet(0x73, wake, 7, UINT32_MAX - 50, 49));
    assert(!battery_keypad_wake_packet(0x73, wake, 7, 1101, 1100));
    assert(bindings.set(40005, 126, ambiguous));
    command.brightness = 255;
    assert(binding_activation_slot(bindings.find(40005), 126, 1200, command) == 0);
    command.brightness = 128;
    assert(binding_activation_slot(bindings.find(40005), 126, 1200, command) == 0);
    assert(binding_context_slot(bindings.find(40005), 126, 1200) == 0);
    bindings.remove(40101);
    assert(!bindings.find(40101));
}
