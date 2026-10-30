#include "controller_events.h"
#include "absolute_dimmer.h"
#include <cassert>
#include <cstring>

// Public native-91 capture: group 272, marker 1, zero byte 5, level at byte 6.
// Intermediate levels below are synthetic boundary/ramp fixtures.
static void test_product91_absolute_variant() {
    using namespace avionmesh;
    ControllerCommand c;
    std::array<uint8_t, 10> on{0, 10, 1, 16, 1, 0, 255, 0, 0, 0};
    for (uint8_t level : {255, 0, 1, 128, 254}) {
        auto bytes = on;
        bytes[6] = level;
        assert(decode_controller_command(91, 0x73, bytes.data(), bytes.size(), c));
        assert(c.absolute && !c.relative && c.target == 272 && c.brightness == level && !c.delta);
        assert(!std::strcmp(c.event_type, level == 0 ? "off" : level == 255 ? "on" : "brightness"));
    }
    for (uint8_t product : {81, 126, 127}) {
        assert(decode_controller_command(product, 0x73, on.data(), on.size(), c));
        assert(!c.absolute && !c.relative && !std::strcmp(c.event_type, "mesh_command"));
    }
    for (uint8_t product : {0, 97, 162, 255}) {
        c.absolute = true;
        assert(!decode_controller_command(product, 0x73, on.data(), on.size(), c));
        assert(!c.absolute && !c.relative && !c.target && !c.brightness);
    }
    // A caller without authenticated model context cannot opt into this layout.
    assert(decode_controller_command(0x73, on.data(), on.size(), c) && !c.absolute);

    // Keep both native compact-controller profiles, including endpoint levels.
    for (uint8_t product : {81, 91}) {
        for (uint8_t level : {0, 128, 255}) {
            auto compact = on;
            compact[4] = 0;
            compact[5] = level;
            compact[6] = 0;
            assert(decode_controller_command(product, 0x73, compact.data(), compact.size(), c));
            assert(c.absolute && c.brightness == level && c.target == 272);
            assert(!decode_controller_command(0, 0x73, compact.data(), compact.size(), c));
            assert(!decode_controller_command(162, 0x73, compact.data(), compact.size(), c));
        }
    }
    // Unsupported lengths, markers, padding and shifted relative forms stay raw.
    for (size_t length = 2; length < on.size(); ++length) {
        assert(decode_controller_command(91, 0x73, on.data(), length, c));
        assert(!c.absolute && !c.relative && !std::strcmp(c.event_type, "mesh_command"));
    }
    std::array<uint8_t, 11> overlong{0, 10, 1, 16, 1, 0, 255, 0, 0, 0, 0};
    assert(decode_controller_command(91, 0x73, overlong.data(), overlong.size(), c) && !c.absolute);
    for (size_t index : {1, 4, 5, 7, 8, 9}) {
        for (uint8_t invalid : {0, 1, 2, 13, 243, 255}) {
            if (invalid == on[index])
                continue;
            auto malformed = on;
            malformed[index] = invalid;
            assert(decode_controller_command(91, 0x73, malformed.data(), malformed.size(), c));
            assert(!c.absolute && !c.relative && !std::strcmp(c.event_type, "mesh_command"));
        }
    }
    for (uint8_t level : {13, 243}) {
        auto relative = on;
        relative[0] = 0x11;
        relative[6] = level;
        assert(decode_controller_command(91, 0x73, relative.data(), relative.size(), c));
        assert(!c.absolute && !c.relative && !std::strcmp(c.event_type, "mesh_command"));
    }
    assert(!decode_controller_command(91, 0x74, on.data(), on.size(), c));
    assert(!decode_controller_command(91, 0x73, on.data(), 0, c));
    assert(!decode_controller_command(91, 0x73, on.data(), 1, c));

    // Direction uses authenticated, ordered commands directly, without load echoes.
    AbsoluteDimmerDirection direction;
    auto observe = [&](uint32_t sequence, uint8_t level, uint32_t received_ms) {
        auto bytes = on;
        bytes[6] = level;
        assert(decode_controller_command(91, 0x73, bytes.data(), bytes.size(), c) && c.absolute);
        return direction.observe(c.target, sequence, c.brightness, received_ms);
    };
    assert(!observe(100, 255, 1000).action);
    auto lower = observe(101, 254, 1100);
    assert(lower.accepted && lower.action && !std::strcmp(lower.action, "lower"));
    assert(!observe(101, 254, 1110).accepted);
    assert(!observe(99, 255, 1120).accepted);
    lower = observe(102, 128, 1200);
    assert(lower.action && lower.difference == -126);
    auto raise = observe(103, 140, 1300);
    assert(raise.action && !std::strcmp(raise.action, "raise") && raise.difference == 12);
    assert(!observe(104, 255, 1400).action);
    assert(!observe(105, 0, 1500).action);
}

static void test_queue_flood_and_recovery() {
    using namespace avionmesh;
    ControllerPacketQueue queue;
    ControllerPacketQueue::Packet packet;
    uint8_t bytes[] = {0x73, 0};
    // Drop newest on saturation, preserving original bytes and receipt times.
    for (size_t i = 0; i < 10 * ControllerPacketQueue::CAPACITY; ++i) {
        bytes[1] = static_cast<uint8_t>(i);
        queue.push(bytes, sizeof(bytes), static_cast<uint32_t>(i));
    }
    bytes[1] = 255;  // Input buffer mutation cannot alter queued packets.
    assert(queue.dropped() == 9 * ControllerPacketQueue::CAPACITY);
    for (size_t i = 0; i < ControllerPacketQueue::CAPACITY / 2; ++i) {
        assert(queue.pop(packet) && packet.received_ms == i && packet.bytes[1] == i);
        assert(!controller_event_is_fresh(packet.received_ms, 2000, 0, 0));
    }
    // Refill across the ring boundary while the surviving older half is pending.
    for (size_t i = 0; i < ControllerPacketQueue::CAPACITY / 2; ++i)
        queue.push(bytes, sizeof(bytes), 1000 + i);
    for (size_t i = ControllerPacketQueue::CAPACITY / 2; i < ControllerPacketQueue::CAPACITY; ++i)
        assert(queue.pop(packet) && packet.received_ms == i && packet.bytes[1] == i);
    for (size_t i = 0; i < ControllerPacketQueue::CAPACITY / 2; ++i)
        assert(queue.pop(packet) && packet.received_ms == 1000 + i && packet.bytes[1] == 255);
    assert(!queue.pop(packet));
    queue.push(bytes, 0, 2000);
    uint8_t oversized[ControllerPacketQueue::MAX_PACKET + 1]{};
    queue.push(oversized, sizeof(oversized), 2001);
    assert(queue.dropped() == 9 * ControllerPacketQueue::CAPACITY + 2 && !queue.pop(packet));
    queue.push(bytes, sizeof(bytes), 2002);
    assert(queue.pop(packet) && packet.received_ms == 2002 && !queue.pop(packet));
}

int main() {
    using namespace avionmesh;
    for (auto product : {81, 91, 126, 127})
        assert(is_controller(product));
    for (auto product : {93, 97, 134})
        assert(!is_controller(product));
    ControllerCommand result;
    uint8_t bytes[] = {0x00, 0x0a, 0x04, 0xb0, 0x00, 0xff, 0, 0, 0, 0};
    assert(decode_controller_command(0x73, bytes, sizeof(bytes), result));
    assert(!std::strcmp(result.event_type, "on") && result.target == 1200);
    assert(result.absolute && result.brightness == 255 && !result.relative);
    bytes[5] = 0;
    assert(decode_controller_command(0x73, bytes, sizeof(bytes), result));
    assert(!std::strcmp(result.event_type, "off") && result.brightness == 0);
    bytes[5] = 128;
    assert(decode_controller_command(0x73, bytes, sizeof(bytes), result));
    assert(!std::strcmp(result.event_type, "brightness") && result.brightness == 128);
    bytes[0] = 0x11;
    bytes[5] = 0x0d;
    assert(decode_controller_command(0x73, bytes, sizeof(bytes), result));
    assert(!std::strcmp(result.event_type, "raise") && result.delta == 13 && !result.absolute);
    bytes[5] = 0xf3;
    assert(decode_controller_command(0x73, bytes, sizeof(bytes), result));
    assert(!std::strcmp(result.event_type, "lower") && result.delta == -13);
    // Unknown layouts/steps stay raw; status/query frames do not become presses.
    bytes[5] = 99;
    assert(decode_controller_command(0x73, bytes, sizeof(bytes), result));
    assert(!std::strcmp(result.event_type, "mesh_command") && !result.relative);
    bytes[0] = 1;
    assert(!decode_controller_command(0x73, bytes, sizeof(bytes), result));
    bytes[0] = 6;
    assert(!decode_controller_command(0x73, bytes, sizeof(bytes), result));
    bytes[0] = 0;
    assert(!decode_controller_command(0x74, bytes, sizeof(bytes), result));
    assert(!decode_controller_command(0x73, bytes, 1, result));
    assert(decode_controller_command(0x73, bytes, 4, result) && !result.absolute);
    bytes[1] = 0x22;
    assert(decode_controller_command(0x73, bytes, sizeof(bytes), result));
    assert(!std::strcmp(result.event_type, "mesh_command"));

    ControllerPacketQueue queue;
    ControllerPacketQueue::Packet packet;
    for (unsigned i = 0; i < 65; ++i)
        queue.push(bytes, sizeof(bytes), i);
    assert(queue.dropped() == 1);
    for (unsigned i = 0; i < 64; ++i) {
        assert(queue.pop(packet) && packet.received_ms == i);
        assert(packet.length == sizeof(bytes) && packet.bytes[1] == 0x22);
    }
    assert(!queue.pop(packet));
    uint8_t oversized[41]{};
    queue.push(oversized, sizeof(oversized), 0);
    assert(queue.dropped() == 2 && !queue.pop(packet));

    ControllerDuplicateWindow duplicates;
    packet.length = 10;
    packet.bytes[0] = 1;  // Stand-in for encrypted packet sequence field.
    assert(duplicates.accept(packet, 0xfffffff0u));
    assert(!duplicates.accept(packet, 0xfffffff1u));
    assert(!duplicates.accept(packet, 0));  // Millisecond wrap preserves dedup.
    packet.bytes[0] = 2;
    assert(duplicates.accept(packet, 1));  // Rapid identical command, new sequence.
    packet.bytes[0] = 1;
    assert(duplicates.accept(packet, 5000));  // Bounded duplicate window.
    assert(controller_event_is_fresh(2000, 2500, 1500, 1500));
    assert(!controller_event_is_fresh(2000, 3001, 1500, 1500));
    assert(!controller_event_is_fresh(2000, 2500, 2001, 1500));
    assert(!controller_event_is_fresh(2000, 2500, 1500, 2001));
    assert(controller_event_is_fresh(0, 10, 0xfffffff0u, 0xfffffff0u));
    test_product91_absolute_variant();
    test_queue_flood_and_recovery();
}
