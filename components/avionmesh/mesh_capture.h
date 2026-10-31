#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace avionmesh {

// Only authenticated, decrypted MCP messages enter this queue. No key material.
// A fixed allocation bounds BLE callback work even if a collector disconnects.
class MeshCapture {
 public:
    static constexpr size_t CAPACITY = 64;
    static constexpr size_t MAX_PAYLOAD = 64;
    struct Packet {
        uint32_t session, received_ms;
        uint16_t mcp_source, crypto_source;
        uint8_t opcode, length;
        size_t original_length;
        std::array<uint8_t, MAX_PAYLOAD> payload;
    };
    struct Status {
        bool active;
        uint32_t session, received, dropped, truncated;
        size_t queued;
    };

    // Durations must be < INT32_MAX ms, allowing wrap-safe deadline comparisons.
    bool start(uint32_t now, uint32_t duration_ms, uint16_t source = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        expire(now);
        if (active_ || count_ || !duration_ms || duration_ms >= 0x80000000u)
            return false;
        ++session_;
        deadline_ = now + duration_ms;
        source_ = source;
        received_ = dropped_ = truncated_ = 0;
        active_ = true;
        return true;
    }
    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = false;  // Preserve already queued packets for draining.
    }
    void push(uint32_t now, uint16_t mcp, uint16_t crypto, uint8_t opcode, const uint8_t *payload,
              size_t length) {
        std::lock_guard<std::mutex> lock(mutex_);
        expire(now);
        if (!active_ || (source_ && source_ != crypto))
            return;
        ++received_;
        if (count_ == CAPACITY) {
            ++dropped_;
            return;
        }
        auto &packet = packets_[(head_ + count_) % CAPACITY];
        packet.session = session_;
        packet.received_ms = now;
        packet.mcp_source = mcp;
        packet.crypto_source = crypto;
        packet.opcode = opcode;
        packet.original_length = length;
        packet.length = length > MAX_PAYLOAD ? MAX_PAYLOAD : length;
        if (length > MAX_PAYLOAD)
            ++truncated_;
        if (packet.length)
            std::memcpy(packet.payload.data(), payload, packet.length);
        ++count_;
    }
    bool pop(Packet &packet) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!count_)
            return false;
        packet = packets_[head_];
        head_ = (head_ + 1) % CAPACITY;
        --count_;
        return true;
    }
    Status status(uint32_t now) {
        std::lock_guard<std::mutex> lock(mutex_);
        expire(now);
        return {active_, session_, received_, dropped_, truncated_, count_};
    }

 private:
    void expire(uint32_t now) {
        if (active_ && static_cast<int32_t>(now - deadline_) >= 0)
            active_ = false;
    }
    std::mutex mutex_;
    std::array<Packet, CAPACITY> packets_{};
    size_t head_{0}, count_{0};
    bool active_{false};
    uint16_t source_{0};
    uint32_t session_{0}, deadline_{0}, received_{0}, dropped_{0}, truncated_{0};
};

}  // namespace avionmesh
