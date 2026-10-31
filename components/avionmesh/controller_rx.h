#pragma once

#include "controller_events.h"
#include <recsrmesh/csrmesh.h>

namespace avionmesh {

// Fit authenticated metadata and the maximum MCP payload (40 - 14 - 3) in
// the existing fixed queue record. No additional queue/window heap or storage.
struct ControllerObservation {
    uint16_t mcp_source{0}, crypto_source{0};
    uint32_t sequence{0};
    uint8_t opcode{0};
    const uint8_t *payload{nullptr};
    size_t payload_len{0};
};
inline constexpr size_t CONTROLLER_OBSERVATION_HEADER = 9;
inline constexpr size_t CONTROLLER_OBSERVATION_MAX_PAYLOAD = 23;
static_assert(csrmesh::MTL_MAX_PACKET == ControllerPacketQueue::MAX_PACKET);
static_assert(CONTROLLER_OBSERVATION_MAX_PAYLOAD ==
              csrmesh::MTL_MAX_PACKET - csrmesh::MCP_CRYPTO_OVERHEAD - csrmesh::MCP_HEADER_LEN);
static_assert(CONTROLLER_OBSERVATION_HEADER + CONTROLLER_OBSERVATION_MAX_PAYLOAD <=
              ControllerPacketQueue::MAX_PACKET);

inline void queue_controller_observation(ControllerPacketQueue &queue,
                                         const ControllerObservation &message,
                                         uint32_t received_ms) {
    if (message.payload_len > CONTROLLER_OBSERVATION_MAX_PAYLOAD ||
        (message.payload_len && !message.payload)) {
        queue.push(nullptr, 0, received_ms);  // Count rejection without dereferencing input.
        return;
    }
    std::array<uint8_t, ControllerPacketQueue::MAX_PACKET> bytes{};
    bytes[0] = message.mcp_source;
    bytes[1] = message.mcp_source >> 8;
    bytes[2] = message.crypto_source;
    bytes[3] = message.crypto_source >> 8;
    for (unsigned i = 0; i < 4; ++i)
        bytes[4 + i] = message.sequence >> (8 * i);
    bytes[8] = message.opcode;
    if (message.payload_len)
        std::memcpy(bytes.data() + CONTROLLER_OBSERVATION_HEADER, message.payload,
                    message.payload_len);
    queue.push(bytes.data(), CONTROLLER_OBSERVATION_HEADER + message.payload_len, received_ms);
}

// The returned payload borrows the popped Packet, not library callback memory.
inline bool parse_controller_observation(const ControllerPacketQueue::Packet &packet,
                                         ControllerObservation &message) {
    message = {};
    if (packet.length < CONTROLLER_OBSERVATION_HEADER ||
        packet.length > CONTROLLER_OBSERVATION_HEADER + CONTROLLER_OBSERVATION_MAX_PAYLOAD)
        return false;
    message.mcp_source = uint16_t(packet.bytes[0]) | (uint16_t(packet.bytes[1]) << 8);
    message.crypto_source = uint16_t(packet.bytes[2]) | (uint16_t(packet.bytes[3]) << 8);
    for (unsigned i = 0; i < 4; ++i)
        message.sequence |= uint32_t(packet.bytes[4 + i]) << (8 * i);
    message.opcode = packet.bytes[8];
    message.payload = packet.bytes.data() + CONTROLLER_OBSERVATION_HEADER;
    message.payload_len = packet.length - CONTROLLER_OBSERVATION_HEADER;
    return true;
}

template <typename NowFn>
inline void install_controller_rx_observer(csrmesh::MeshContext &ctx, ControllerPacketQueue &queue,
                                           NowFn now) {
#if defined(RECSRMESH_RX_OBSERVER_API) && RECSRMESH_RX_OBSERVER_API >= 1
    (void)now;
    csrmesh::set_rx_observer(ctx, [&queue](const csrmesh::RxMessage &message) {
        queue_controller_observation(queue,
                                     {message.mcp_source, message.crypto_source, message.sequence,
                                      message.opcode, message.payload, message.payload_len},
                                     message.received_ms);
    });
#else
    // Released v1.1.3 lacks the observer API. Retain the existing internal MTL
    // wrapper until a released dependency can be pinned; review concern remains.
    auto original = ctx.mtl.rx_cb;
    ctx.mtl.rx_cb = [&queue, original, now](const uint8_t *data, size_t length) {
        queue.push(data, length, now());
        if (original)
            original(data, length);
    };
#endif
}

}  // namespace avionmesh
