#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

#ifdef AVIONMESH_TEST_RX_OBSERVER
#define RECSRMESH_RX_OBSERVER_API 1
#endif

namespace csrmesh {

enum class Error {
    Ok = 0,
    InvalidArg,
    BufferTooSmall,
    Crypto,
    HmacMismatch,
    PacketTooShort,
    BleWrite,
    ProtoState,
    Ecdh,
    Busy,
};

enum class Characteristic { Low = 0, High = 1 };

inline constexpr size_t KEY_LEN = 16;
inline constexpr size_t MTL_FRAG_SIZE = 20;
inline constexpr size_t MTL_MAX_PACKET = 40;
inline constexpr uint16_t MCP_CRYPTO_SOURCE = 0x8000;
inline constexpr uint8_t MCP_MODEL_OPCODE = 0x73;
inline constexpr size_t MCP_CRYPTO_OVERHEAD = 14;
inline constexpr size_t MCP_HEADER_LEN = 3;

using ByteSpan = std::span<const uint8_t>;
using RxCallback = std::function<void(uint16_t, uint16_t, uint8_t, const uint8_t *, size_t)>;
using DiscoveryCallback = std::function<void(const uint8_t *, size_t, uint32_t)>;

// --- mtl ---
namespace mtl {
using BleWriteFn = std::function<int(Characteristic, const uint8_t *, size_t, bool)>;
using RxCallback = std::function<void(const uint8_t *, size_t)>;
struct Context {
    BleWriteFn ble_write;
    RxCallback rx_cb = [](const uint8_t *, size_t) {};
};
inline void init(Context &, BleWriteFn, RxCallback) {}
inline void feed(Context &, Characteristic, const uint8_t *, size_t, uint32_t) {}
inline void poll(Context &, uint32_t) {}
inline Error send(Context &, const uint8_t *, size_t, bool) {
    return Error::Ok;
}
}  // namespace mtl

// --- masp (opaque stub) ---
namespace masp {
struct Context {};
}  // namespace masp

// --- protocol ---
namespace protocol {
enum class State { Init = 0, WaitAssocResponse, PubkeyExchange, Complete, Error };
struct Context {
    masp::Context assoc;
    State state = State::Init;
    uint16_t device_id = 0;
    const char *error = nullptr;
};
inline Error init(Context &, uint32_t, uint16_t, const char *) {
    return Error::Ok;
}
inline size_t get_initial_request(Context &, uint8_t *, size_t) {
    return 0;
}
inline size_t process(Context &, const uint8_t *, size_t, uint8_t *, size_t) {
    return 0;
}
inline bool is_complete(const Context &) {
    return false;
}
inline bool is_error(const Context &) {
    return false;
}
inline const char *state_name(State) {
    return "stub";
}
inline void cleanup(Context &) {}
}  // namespace protocol

#ifdef AVIONMESH_TEST_RX_OBSERVER
struct RxMessage {
    uint16_t mcp_source, crypto_source;
    uint32_t sequence;
    uint8_t opcode;
    const uint8_t *payload;
    size_t payload_len;
    uint32_t received_ms;
};
using RxObserver = std::function<void(const RxMessage &)>;
#endif

// --- MeshContext ---
struct MeshContext {
    mtl::Context mtl;
    mtl::BleWriteFn ble_write;
    uint8_t mcp_key[KEY_LEN] = {};
    uint8_t masp_key[KEY_LEN] = {};
    RxCallback rx_cb;
#ifdef AVIONMESH_TEST_RX_OBSERVER
    RxObserver rx_observer;
#endif
    bool discovering = false;
    DiscoveryCallback discovery_cb;
    protocol::Context *proto = nullptr;
    uint32_t mcp_seq = 0;
};

inline Error init(MeshContext &, mtl::BleWriteFn, const char *) {
    return Error::Ok;
}
inline void set_rx_callback(MeshContext &ctx, RxCallback cb) {
    ctx.rx_cb = std::move(cb);
}
#ifdef AVIONMESH_TEST_RX_OBSERVER
inline void set_rx_observer(MeshContext &ctx, RxObserver observer) {
    ctx.rx_observer = std::move(observer);
}
#endif
inline void feed_notify(MeshContext &, Characteristic, const uint8_t *, size_t, uint32_t) {}
inline void poll(MeshContext &, uint32_t) {}
inline Error send(MeshContext &, uint16_t, uint8_t, const uint8_t *, size_t) {
    return Error::Ok;
}
inline Error disassociate(MeshContext &, uint16_t) {
    return Error::Ok;
}
inline Error discover_start(MeshContext &ctx, DiscoveryCallback cb) {
    ctx.discovery_cb = std::move(cb);
    return Error::Ok;
}
inline void discover_stop(MeshContext &) {}
inline Error associate_start(MeshContext &, protocol::Context *, uint32_t, uint16_t) {
    return Error::Ok;
}
inline void associate_cancel(MeshContext &) {}

}  // namespace csrmesh
