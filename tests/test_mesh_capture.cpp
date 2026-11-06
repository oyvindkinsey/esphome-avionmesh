#include "mesh_capture.h"
#include <cassert>
#include <thread>

int main() {
    using avionmesh::MeshCapture;
    MeshCapture capture;
    MeshCapture::Packet packet;
    const uint8_t bytes[] = {0, 10, 2, 244, 0, 255};
    capture.push(0, 0, 40101, 115, bytes, sizeof(bytes));
    assert(!capture.pop(packet));  // Inactive by default.
    assert(capture.start(100, 1000, 40101));
    assert(!capture.start(100, 1000));  // Don't overwrite a running session.
    capture.push(101, 0, 99, 115, bytes, sizeof(bytes));
    assert(capture.status(101).received == 0);  // Filter uses authenticated source.
    for (unsigned i = 0; i < MeshCapture::CAPACITY + 2; ++i)
        capture.push(102 + i, 0x8000, 40101, 115, bytes, sizeof(bytes));
    auto status = capture.status(200);
    assert(status.received == 66 && status.dropped == 2 && status.queued == 64);
    capture.stop();
    capture.push(201, 0, 40101, 115, bytes, sizeof(bytes));
    for (unsigned i = 0; i < MeshCapture::CAPACITY; ++i) {
        assert(capture.pop(packet));
        assert(packet.received_ms == 102 + i && packet.crypto_source == 40101);
        assert(packet.mcp_source == 0x8000 && packet.opcode == 115);
        assert(packet.length == sizeof(bytes) && packet.payload[5] == 255);
    }
    assert(!capture.pop(packet));  // Identical rapid repeats have all been retained.
    assert(capture.start(0xfffffff0u, 32));
    uint8_t oversized[80]{};
    capture.push(0, 7, 8, 115, oversized, sizeof(oversized));
    assert(capture.pop(packet) && packet.length == 64 && packet.original_length == 80);
    assert(capture.status(0).active && capture.status(0).truncated == 1);
    capture.push(16, 7, 8, 115, bytes, sizeof(bytes));
    assert(!capture.status(16).active && !capture.pop(packet));  // Wrap-safe expiry.
    assert(capture.start(20, 10000));
    std::thread producer([&]() {
        for (unsigned i = 0; i < 10000; ++i)
            capture.push(21, 0, 40101, 115, bytes, sizeof(bytes));
    });
    unsigned drained = 0;
    while (capture.status(21).received < 10000) {
        if (capture.pop(packet))
            ++drained;
    }
    producer.join();
    while (capture.pop(packet))
        ++drained;
    status = capture.status(21);
    assert(status.received == 10000 && drained + status.dropped == 10000);
}
