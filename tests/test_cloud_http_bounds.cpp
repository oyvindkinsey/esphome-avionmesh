#include "cloud_http_bounds.h"
#include <cassert>
#include <limits>
#include <vector>

using namespace avionmesh;

// Models IDF's ignored event-handler return and synchronous close->DISCONNECTED.
struct FakeTransport {
    explicit FakeTransport(int64_t deadline) : response(deadline) {}
    CloudHttpResponseBound response;
    unsigned closes{0}, callback_depth{0}, max_depth{0};
    bool closed{false};
    std::vector<int> timeouts;
    void dispatch(CloudHttpEvent event, const char *bytes, int64_t length, int64_t now) {
        ++callback_depth;
        if (callback_depth > max_depth)
            max_depth = callback_depth;
        (void)handle_cloud_http_event(response, event, bytes, length, now,
            [&](int ms) { assert(ms > 0 && ms <= 15000); timeouts.push_back(ms); },
            [&] {
                assert(response.close_started);
                ++closes;
                closed = true;
                // The SDK reuses the event during synchronous dispatch. No recursion.
                dispatch(CloudHttpEvent::Terminal, nullptr, 0, now);
            });
        --callback_depth;
    }
};

int main() {
    const std::string cap(CloudHttpResponseBound::MAX_BODY, 'x');
    FakeTransport exact(1000000);
    exact.dispatch(CloudHttpEvent::Data, cap.data(), cap.size(), 1);
    assert(exact.response.body == cap && exact.closes == 0);
    assert(exact.response.complete(true, true, 2));
    exact.dispatch(CloudHttpEvent::Data, "!", 1, 3);
    assert(exact.closed && exact.closes == 1 && exact.max_depth == 2);
    assert(exact.response.too_big && exact.response.body.size() == cap.size());
    exact.dispatch(CloudHttpEvent::Data, "more", 4, 4); // Already-buffered parser events.
    exact.dispatch(CloudHttpEvent::Terminal, nullptr, 0, 2000000);
    assert(exact.closes == 1 && exact.response.body == cap);
    assert(!exact.response.complete(true, true, 5)); // Even a false-success transport cannot commit.

    FakeTransport oversize(1000000);
    oversize.dispatch(CloudHttpEvent::Data, cap.data(), cap.size() + 1, 0);
    assert(oversize.response.too_big && oversize.response.body.empty() && oversize.closes == 1);
    FakeTransport invalid(1000000);
    invalid.dispatch(CloudHttpEvent::Data, nullptr, -1, 0);
    assert(invalid.response.too_big && invalid.closes == 1);
    FakeTransport null_data(1000000);
    null_data.dispatch(CloudHttpEvent::Data, nullptr, 1, 0);
    assert(null_data.response.too_big && null_data.closes == 1);

    FakeTransport trickle(180000000);
    trickle.dispatch(CloudHttpEvent::Connected, nullptr, 0, 0);
    trickle.dispatch(CloudHttpEvent::HeadersSent, nullptr, 0, 1000);
    trickle.dispatch(CloudHttpEvent::Header, nullptr, 0, 1000000);
    assert(trickle.timeouts.back() == 15000);
    for (int64_t now = 10000000; now < 180000000; now += 10000000)
        trickle.dispatch(CloudHttpEvent::Data, " ", 1, now);
    trickle.dispatch(CloudHttpEvent::Header, nullptr, 0, 179998999);
    assert(trickle.timeouts.back() == 2);
    trickle.dispatch(CloudHttpEvent::Data, " ", 1, 179999999);
    assert(trickle.timeouts.back() == 1);
    const auto before_timeout = trickle.response.body;
    trickle.dispatch(CloudHttpEvent::Data, " ", 1, 180000000);
    assert(trickle.response.timed_out && trickle.closes == 1 && trickle.closed);
    trickle.dispatch(CloudHttpEvent::Data, " ", 1, 180000001);
    assert(trickle.response.body == before_timeout && trickle.closes == 1);
    assert(!trickle.response.complete(true, true, 180000001));

    FakeTransport terminal(100);
    terminal.dispatch(CloudHttpEvent::Terminal, nullptr, 0, 200);
    assert(terminal.closes == 0); // FINISH/DISCONNECTED never initiate close.
    assert(!terminal.response.complete(true, true, 200)); // Final check catches silent expiry.
    assert(terminal.response.timed_out);
    FakeTransport truncated(1000);
    truncated.dispatch(CloudHttpEvent::Data, "{}", 2, 1); // Valid JSON prefix is not a full HTTP body.
    assert(!truncated.response.complete(true, false, 2));
    assert(!truncated.response.complete(false, true, 2));
    assert(truncated.response.complete(true, true, 2));
    CloudHttpResponseBound expired(0);
    assert(expired.io_timeout_ms(0) == 0 && expired.timed_out);
    CloudHttpResponseBound enormous(std::numeric_limits<int64_t>::max());
    assert(enormous.io_timeout_ms(0) == 15000);
}
