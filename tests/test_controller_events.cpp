#include "controller_events.h"
#include <cassert>
#include <cstring>

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
}
