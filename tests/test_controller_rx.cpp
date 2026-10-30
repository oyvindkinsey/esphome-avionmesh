#include "controller_rx.h"
#include <cassert>
#include <thread>
#include <atomic>

using namespace avionmesh;
int main() {
    ControllerPacketQueue queue;
    ControllerPacketQueue::Packet packet;
    uint8_t payload[23]{};
    payload[0] = 0xff;
    ControllerObservation source{272, 0x8123, 0xabcdef, 0x73, payload, sizeof(payload)};
    queue_controller_observation(queue, source, 1234);
    payload[0] = 0;
    assert(queue.pop(packet) && packet.received_ms == 1234);
    ControllerObservation parsed;
    assert(parse_controller_observation(packet, parsed));
    assert(parsed.mcp_source == 272 && parsed.crypto_source == 0x8123);
    assert(parsed.sequence == 0xabcdef && parsed.opcode == 0x73);
    assert(parsed.payload_len == 23 && parsed.payload[0] == 0xff);
    ControllerDuplicateWindow duplicates;
    assert(duplicates.accept(packet, 1234));
    assert(!duplicates.accept(packet, 1235));
    source.crypto_source = 0x8124;
    queue_controller_observation(queue, source, 1235);
    assert(queue.pop(packet) && duplicates.accept(packet, 1235));
    source.sequence++;
    queue_controller_observation(queue, source, 1236);
    assert(queue.pop(packet) && duplicates.accept(packet, 1236));
    packet.length = 8;
    assert(!parse_controller_observation(packet, parsed));
    packet.length = 33;
    assert(!parse_controller_observation(packet, parsed));
    source.payload_len = 24;
    queue_controller_observation(queue, source, 0);
    source.payload_len = 1;
    source.payload = nullptr;
    queue_controller_observation(queue, source, 0);
    assert(queue.dropped() == 2 && !queue.pop(packet));
    source.payload_len = 0;
    queue_controller_observation(queue, source, 0);
    assert(queue.pop(packet) && parse_controller_observation(packet, parsed) && !parsed.payload_len);

    csrmesh::MeshContext ctx;
    unsigned native_calls = 0;
    ctx.mtl.rx_cb = [&](const uint8_t *, size_t) { ++native_calls; };
    install_controller_rx_observer(ctx, queue, [] { return 4321u; });
#if defined(RECSRMESH_RX_OBSERVER_API) && RECSRMESH_RX_OBSERVER_API >= 1
    assert(ctx.rx_observer);
    ctx.mtl.rx_cb(payload, 23);
    assert(native_calls == 1 && !queue.pop(packet)); // Native handler was untouched.
    ctx.rx_observer({272, 0x8123, 321, 0x73, payload, 23, 999});
    assert(queue.pop(packet) && packet.received_ms == 999);
    assert(parse_controller_observation(packet, parsed) && parsed.sequence == 321);
    for (unsigned i = 0; i < 65; ++i)
        ctx.rx_observer({272, 0x8123, i, 0x73, payload, 23, 999});
    assert(queue.dropped() == 3);
#else
    ctx.mtl.rx_cb(payload, 23);
    assert(native_calls == 1 && queue.pop(packet) && packet.received_ms == 4321);
    assert(packet.length == 23);
    for (unsigned i = 0; i < 65; ++i)
        ctx.mtl.rx_cb(payload, 23);
    assert(native_calls == 66 && queue.dropped() == 3); // Overflow cannot suppress native RX.
#endif
    queue.clear();
    assert(!queue.pop(packet) && queue.dropped() == 3);
    queue_controller_observation(queue, {272, 0x8123, 111, 0x73, payload, 23}, 100);
    queue.clear(); // A mesh-key change must not replay a snapshot authenticated with the old key.
    assert(!queue.pop(packet));
    queue_controller_observation(queue, {272, 0x8124, 222, 0x73, payload, 23}, 200);
    assert(queue.pop(packet) && parse_controller_observation(packet, parsed));
    assert(parsed.sequence == 222 && parsed.crypto_source == 0x8124);
    // Producer and consumer may run on BLE/main-loop threads. Metadata must not tear.
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (uint32_t i = 0; i < 10000; ++i)
            queue_controller_observation(queue, {272, 0x8123, i, 0x73, payload, 23}, i);
        done = true;
    });
    do {
        while (queue.pop(packet)) {
            assert(parse_controller_observation(packet, parsed));
            assert(parsed.sequence == packet.received_ms && parsed.crypto_source == 0x8123);
        }
    } while (!done);
    producer.join();
    while (queue.pop(packet)) {
        assert(parse_controller_observation(packet, parsed));
        assert(parsed.sequence == packet.received_ms);
    }
}
