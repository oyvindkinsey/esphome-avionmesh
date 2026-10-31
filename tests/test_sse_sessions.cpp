#include "sse_initial_sync.h"
#include "device_db.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <map>
#include <set>
#include <thread>

using namespace avionmesh;
namespace {
constexpr int TIMEOUT = -3;
struct Wire {
    std::string bytes;
    unsigned sends{0}, closes{0};
    bool blocked{false};
    int send(const SseSession &, const char *data, size_t length) {
        ++sends;
        if (blocked || sends % 5 == 0)
            return TIMEOUT;
        const size_t used = std::min(size_t(17), length);
        bytes.append(data, used);
        return static_cast<int>(used);
    }
    bool close(const SseSession &) {
        ++closes;
        return true;
    }
    void drain(SseSessions &sessions) {
        sessions.drain([&](const auto &s, auto p, auto n) { return send(s, p, n); },
                       [&](const auto &s) { return close(s); }, TIMEOUT);
    }
};
struct Event {
    std::string type, json;
};
std::vector<Event> decode(const std::string &wire) {
    std::vector<Event> events;
    size_t pos = 0;
    while (pos < wire.size()) {
        assert(wire.substr(pos + 8, 2) == "\r\n");
        const size_t length = std::stoul(wire.substr(pos, 8), nullptr, 16);
        const auto body = wire.substr(pos + 10, length);
        assert(body.size() == length && wire.substr(pos + 10 + length, 2) == "\r\n");
        const auto event_end = body.find("\r\n");
        assert(body.rfind("event: ", 0) == 0);
        assert(body.substr(event_end + 2, 6) == "data: ");
        assert(body.substr(body.size() - 4) == "\r\n\r\n");
        events.push_back({body.substr(7, event_end - 7),
                          body.substr(event_end + 8, body.size() - event_end - 12)});
        pos += 12 + length;
    }
    return events;
}
void status_backpressure() {
    SseSessions sessions;
    auto session = sessions.accept(1, 10);
    auto context = std::make_unique<SseContext>(session);
    Wire wire;
    sessions.broadcast("controller_buttons", "{\"status\":\"waiting_for_wake\"}");
    wire.blocked = true;
    wire.drain(sessions);
    sessions.broadcast("controller_buttons", "{\"status\":\"reading\"}");
    sessions.broadcast("controller_buttons", "{\"status\":\"saved\"}");
    sessions.visit([](auto &s) { assert(s.buffered_frames() == 3 && !s.closing()); });
    wire.blocked = false;
    for (unsigned i = 0; i < 1000; ++i)
        wire.drain(sessions);
    const auto events = decode(wire.bytes);
    assert(events.size() == 3 && wire.closes == 0);
    assert(events[0].json.find("waiting_for_wake") != std::string::npos);
    assert(events[1].json.find("reading") != std::string::npos);
    assert(events[2].json.find("saved") != std::string::npos);
}
void lifecycle_and_errors() {
    SseContext *survivor;
    std::weak_ptr<SseSession> weak;
    {
        SseSessions sessions;
        auto session = sessions.accept(1, 1);
        weak = session;
        survivor = new SseContext(session);
    }
    assert(!weak.expired());  // IDF context outlives manager safely.
    delete survivor;
    assert(weak.expired());

    // Server stops after close-queue failures: context remains the final owner.
    auto stopped = std::make_unique<SseSessions>();
    auto stopped_context = std::make_unique<SseContext>(stopped->accept(1, 8));
    weak = stopped_context->session;
    stopped->broadcast("meta", "{}");
    unsigned failed_closes = 0;
    stopped->drain([](const auto &, auto, auto) { return -1; },
                   [&](const auto &) {
                       ++failed_closes;
                       return false;
                   },
                   TIMEOUT);
    assert(failed_closes == 1);
    stopped.reset();
    assert(!weak.expired());
    stopped_context.reset();  // IDF stop's eventual free_ctx, with no manager left.
    assert(weak.expired());

    SseSessions scheduler;
    unsigned submissions = 0;
    scheduler.schedule(1, [&](uintptr_t) {
        ++submissions;
        return false;
    });
    scheduler.schedule(1, [&](uintptr_t) {
        ++submissions;
        return true;
    });
    scheduler.schedule(1, [&](uintptr_t) {
        ++submissions;
        return true;
    });
    assert(submissions == 2);  // Full mailbox retries; accepted work is not duplicated.
    scheduler.finish_work();
    scheduler.schedule(1, [&](uintptr_t) {
        ++submissions;
        return true;
    });
    scheduler.schedule(0, [&](uintptr_t) {
        assert(false);
        return true;
    });
    scheduler.schedule(2, [&](uintptr_t) {
        ++submissions;
        return true;
    });
    assert(submissions == 4);  // Lost queued work from a stopped server cannot wedge a new one.

    SseSessions sessions;
    std::vector<std::unique_ptr<SseContext>> contexts;
    for (int fd = 1; fd <= 3; ++fd)
        contexts.push_back(std::make_unique<SseContext>(sessions.accept(1, fd)));
    assert(!sessions.accept(1, 4));  // Reject new; do not delete/evict attached context.
    std::weak_ptr<SseSession> old = contexts[0]->session;
    contexts[0].reset();  // Actual production free-context ownership path.
    sessions.visit([](auto &) {});
    assert(old.expired());
    auto fourth = sessions.accept(1, 4);
    assert(fourth);
    contexts[0] = std::make_unique<SseContext>(fourth);

    for (int error : {-1, -2, 0}) {
        SseSessions failed;
        auto context = std::make_unique<SseContext>(failed.accept(1, 20));
        failed.broadcast("controller_buttons", "{}");
        unsigned closes = 0;
        for (unsigned i = 0; i < 10; ++i)
            failed.drain([&](const auto &, auto, auto) { return error; },
                         [&](const auto &) {
                             ++closes;
                             return true;
                         },
                         TIMEOUT);
        assert(closes == 1 && context->session->closing());
        failed.broadcast("controller_buttons", "{\"status\":\"saved\"}");
        failed.visit([](auto &s) { assert(s.buffered_bytes() == 0); });
    }
    SseSessions full;
    auto context = std::make_unique<SseContext>(full.accept(1, 30));
    for (size_t i = 0; i <= SseSession::MAX_FRAMES; ++i)
        full.broadcast("controller_buttons", "{}");
    unsigned close_calls = 0;
    full.drain(
        [](const auto &, auto, auto) {
            assert(false);
            return 0;
        },
        [&](const auto &) {
            ++close_calls;
            return false;
        },
        TIMEOUT);
    assert(close_calls == 1 && !context->session->released());
    full.drain(
        [](const auto &, auto, auto) {
            assert(false);
            return 0;
        },
        [&](const auto &) {
            ++close_calls;
            return true;
        },
        TIMEOUT);
    full.drain(
        [](const auto &, auto, auto) {
            assert(false);
            return 0;
        },
        [&](const auto &) {
            ++close_calls;
            return true;
        },
        TIMEOUT);
    assert(close_calls == 2);  // Retry failed queue submission, never duplicate accepted close.
    context.reset();           // Delayed IDF cleanup remains safe after overflow and close.
    full.visit([](auto &) { assert(false); });

    SseSessions bytes_full;
    auto byte_context = std::make_unique<SseContext>(bytes_full.accept(1, 33));
    bytes_full.broadcast("meta", std::string(4000, 'x'));
    bytes_full.broadcast("meta", std::string(4000, 'x'));
    assert(!byte_context->session->closing());
    bytes_full.broadcast("meta", std::string(4000, 'x'));
    assert(byte_context->session->closing() && byte_context->session->buffered_bytes() == 0);
    SseSessions oversized;
    auto large = std::make_unique<SseContext>(oversized.accept(1, 31));
    oversized.broadcast("groups", std::string(SseSession::MAX_BUFFERED_BYTES, 'x'));
    assert(large->session->closing() && large->session->buffered_bytes() == 0);
    SseSessions stuck;
    auto waiting = std::make_unique<SseContext>(stuck.accept(1, 32));
    stuck.broadcast("meta", "{}");
    Wire blocked;
    blocked.blocked = true;
    for (unsigned i = 0; i < SseSession::MAX_TIMEOUTS + 5; ++i)
        blocked.drain(stuck);
    assert(blocked.closes == 1 && waiting->session->closing());
}
struct State {
    bool brightness_known{true}, color_temp_known{true};
    unsigned brightness{100}, color_temp{300};
};
struct Inventory {
    std::vector<DeviceEntry> devs;
    std::vector<GroupEntry> grps;
    const auto &devices() const { return devs; }
    const auto &groups() const { return grps; }
};
void initial_sync(size_t device_count, size_t group_count, bool dense, bool heavy_names = false) {
    Inventory db;
    const uint16_t group_base = heavy_names ? 20000 : 256;
    const std::string tricky_name =
        "quoted \"name\" \\ path\nnewline\rreturn\ttab\x01\x1f café 灯 ";
    std::map<uint16_t, State> states;
    for (size_t i = 0; i < device_count; ++i) {
        const auto id = static_cast<uint16_t>(32768 + i);
        db.devs.push_back({id,
                           static_cast<uint8_t>(i % 5 ? 93 : 127),
                           tricky_name + "device " + std::to_string(i),
                           {}});
        if (heavy_names)
            db.devs.back().name = std::string(128, '\x01');
        states[id] = {};
    }
    size_t expected_members = 0;
    for (size_t g = 0; g < group_count; ++g) {
        const auto id = static_cast<uint16_t>(group_base + g);
        GroupEntry group{id, tricky_name + "group " + std::to_string(g), {}};
        for (size_t i = 0; i < (dense ? device_count : size_t(4)); ++i) {
            auto &device = db.devs[i % device_count];
            group.member_ids.push_back(device.avion_id);
            device.groups.push_back(id);
            ++expected_members;
        }
        db.grps.push_back(std::move(group));
    }
    SseSessions sessions;
    auto context = std::make_unique<SseContext>(sessions.accept(1, 40));
    Wire wire;
    for (unsigned turn = 0; turn < 100000; ++turn) {
        bool complete = false;
        sessions.visit([&](auto &session) {
            if (session.sync_pending)
                send_sse_initial_sync(
                    &session, db, states, "{}", false,
                    [](uint16_t id) { return "{\"controller_id\":" + std::to_string(id) + "}"; },
                    [](uint16_t) { return std::string("fixture"); });
            assert(session.buffered_bytes() <= SseSession::MAX_BUFFERED_BYTES);
            assert(session.buffered_frames() <= SseSession::MAX_FRAMES && !session.closing());
            complete = !session.sync_pending && session.buffered_bytes() == 0;
        });
        if (complete)
            break;
        wire.drain(sessions);
        assert(turn < 99999);
    }
    const auto events = decode(wire.bytes);
    assert(!events.empty() && events.back().type == "sync_complete" && wire.closes == 0);
    std::set<uint16_t> devices, groups;
    size_t members = 0, complete_count = 0;
    for (const auto &event : events) {
        JsonDocument doc;
        assert(!deserializeJson(doc, event.json));
        if (event.type == "devices")
            for (JsonObjectConst device : doc["devices"].as<JsonArrayConst>()) {
                const auto id = device["avion_id"].as<uint16_t>();
                assert(devices.insert(id).second);
                assert(device["name"].as<std::string>() == db.devs.at(id - 32768).name);
            }
        if (event.type == "groups")
            for (JsonObjectConst group : doc["groups"].as<JsonArrayConst>()) {
                const auto id = group["group_id"].as<uint16_t>();
                assert(groups.insert(id).second);
                assert(group["name"].as<std::string>() == db.grps.at(id - group_base).name);
                members += group["members"].size();
            }
        if (event.type == "sync_complete") {
            ++complete_count;
            assert(devices.size() == device_count && groups.size() == group_count &&
                   members == expected_members);
        }
    }
    assert(complete_count == 1);
}
void concurrent_producers() {
    SseSessions sessions;
    std::atomic<bool> done{false};
    std::atomic<unsigned> sends{0};
    std::thread httpd([&] {
        std::vector<std::unique_ptr<SseContext>> contexts;
        for (unsigned turn = 0; !done || turn < 1000; ++turn) {
            if (auto session = sessions.accept(1, 100 + turn))
                contexts.push_back(std::make_unique<SseContext>(session));
            std::set<int> closed;
            sessions.drain(
                [&](const auto &, auto, size_t length) {
                    ++sends;
                    return static_cast<int>(length);
                },
                [&](const auto &session) {
                    closed.insert(session.fd);
                    return true;
                },
                TIMEOUT);
            for (auto it = contexts.begin(); it != contexts.end();) {
                if (closed.count((*it)->session->fd) || turn % 7 == 0)
                    it = contexts.erase(it);  // Serialized HTTPD cleanup.
                else
                    ++it;
            }
        }
    });
    for (unsigned i = 0; i < 10000; ++i) {
        sessions.broadcast("controller_buttons", "{\"status\":\"reading\"}");
        sessions.visit(
            [](auto &s) { assert(s.buffered_bytes() <= SseSession::MAX_BUFFERED_BYTES); });
        if (i % 100 == 0)
            sessions.reset_sync();
    }
    done = true;
    httpd.join();
    sessions.visit([](auto &) { assert(false); });
}
}  // namespace
int main() {
    status_backpressure();
    lifecycle_and_errors();
    initial_sync(19, 10, false);
    initial_sync(81, 16, false);
    initial_sync(200, 40, true);
    initial_sync(5, 128, true,
                 true);  // Each record fits; five escaped names/membership arrays do not.
    concurrent_producers();
}
