#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace avionmesh {

// SDK-independent response policy. Failure is sticky even when the transport
// ignores callback return values or has already buffered more parser events.
struct CloudHttpResponseBound {
    static constexpr size_t MAX_BODY = 262144;
    static constexpr int MAX_IO_TIMEOUT_MS = 15000;
    explicit CloudHttpResponseBound(int64_t deadline_us) : deadline_us(deadline_us) {}

    std::string body;
    const int64_t deadline_us;
    bool too_big{false}, timed_out{false}, close_started{false};

    bool failed() const { return too_big || timed_out; }
    bool check_budget(int64_t now_us) {
        if (now_us >= deadline_us)
            timed_out = true;
        return !failed();
    }
    int io_timeout_ms(int64_t now_us) {
        if (!check_budget(now_us))
            return 0;
        const int64_t remaining = deadline_us - now_us;
        if (remaining >= int64_t(MAX_IO_TIMEOUT_MS) * 1000)
            return MAX_IO_TIMEOUT_MS;
        // Round up to a positive millisecond for sub-ms remaining budgets.
        return static_cast<int>((remaining + 999) / 1000);
    }
    bool append(const char *data, int64_t length, int64_t now_us) {
        if (!check_budget(now_us))
            return false;
        if (length < 0 || (length && !data) ||
            static_cast<uint64_t>(length) > MAX_BODY - body.size()) {
            too_big = true;
            return false;
        }
        if (length)
            body.append(data, static_cast<size_t>(length));
        return true;
    }
    bool begin_close() {
        if (!failed() || close_started)
            return false;
        close_started = true;  // Must precede synchronous DISCONNECTED dispatch.
        return true;
    }
    bool complete(bool transport_ok, bool framing_complete, int64_t now_us) {
        return check_budget(now_us) && transport_ok && framing_complete;
    }
};

enum class CloudHttpEvent { Connected, HeadersSent, Header, Data, Terminal };

template <typename SetTimeout, typename Close>
bool handle_cloud_http_event(CloudHttpResponseBound &response, CloudHttpEvent event,
                             const char *data, int64_t length, int64_t now_us,
                             SetTimeout set_timeout, Close close) {
    // In particular, close() synchronously dispatches DISCONNECTED. Never close
    // from terminal/error callbacks or depend on their return values to abort.
    if (event == CloudHttpEvent::Terminal)
        return !response.failed();
    const int timeout = response.io_timeout_ms(now_us);
    if (timeout)
        set_timeout(timeout);
    if (event == CloudHttpEvent::Data && timeout)
        response.append(data, length, now_us);
    if (response.begin_close())
        close();
    return !response.failed();
}

}  // namespace avionmesh
