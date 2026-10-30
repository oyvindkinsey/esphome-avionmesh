#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace avionmesh {

// Socket I/O/close is serviced only on the HTTPD task. Main-loop producers only
// enqueue, under SseSessions' mutex. IDF owns a separate SseContext reference.
class SseSession {
 public:
    static constexpr size_t MAX_BUFFERED_BYTES = 8192, MAX_FRAMES = 16;
    static constexpr unsigned MAX_TIMEOUTS = 2500;
    enum class SyncPhase { Meta, Devices, Controllers, Groups, Mesh, Complete };
    const uintptr_t server;
    const int fd;
    bool sync_pending{true};
    SyncPhase sync_phase{SyncPhase::Meta};
    size_t sync_index{0};

    SseSession(uintptr_t server, int fd) : server(server), fd(fd) {}
    bool released() const { return released_.load(); }
    bool closing() const { return closing_; }
    bool ready_for_sync() const { return !closing_ && !released() && frames_.empty(); }
    size_t buffered_bytes() const { return buffered_bytes_; }
    size_t buffered_frames() const { return frames_.size(); }
    void restart_sync() {
        sync_pending = true;
        sync_phase = SyncPhase::Meta;
        sync_index = 0;
    }
    void close() {
        closing_ = true;
        frames_.clear();
        buffered_bytes_ = sent_ = 0;
    }
    bool send(const char *event, const std::string &data) {
        if (closing_ || released())
            return false;
        const std::string event_name = event ? event : "";
        // Bound before allocating the wire frame; an oversized single event is
        // explicitly disconnected, never silently skipped or retried forever.
        const size_t overhead = 10 + 2 + 6 + 4 + (event_name.empty() ? 0 : 9 + event_name.size());
        if (overhead > MAX_BUFFERED_BYTES || data.size() > MAX_BUFFERED_BYTES - overhead ||
            frames_.size() == MAX_FRAMES ||
            data.size() + overhead > MAX_BUFFERED_BYTES - buffered_bytes_) {
            close();
            return false;
        }
        std::string frame;
        frame.reserve(data.size() + overhead);
        frame = "        \r\n";
        if (!event_name.empty())
            frame += "event: " + event_name + "\r\n";
        frame += "data: " + data + "\r\n\r\n\r\n";
        char length[9];
        std::snprintf(length, sizeof(length), "%08x", static_cast<unsigned>(frame.size() - 12));
        frame.replace(0, 8, length, 8);
        buffered_bytes_ += frame.size();
        frames_.push_back(std::move(frame));
        return true;
    }

 private:
    friend struct SseContext;
    friend class SseSessions;
    std::atomic<bool> released_{false};
    std::deque<std::string> frames_;
    size_t buffered_bytes_{0}, sent_{0};
    unsigned timeouts_{0};
    bool closing_{false}, close_queued_{false};

    template <typename Send, typename Close>
    void drain(Send send, Close close_socket, int timeout_error) {
        if (released())
            return;
        for (size_t count = 0; !closing_ && !frames_.empty() && count < MAX_FRAMES; ++count) {
            auto &frame = frames_.front();
            const size_t remaining = frame.size() - sent_;
            const int result = send(*this, frame.data() + sent_, remaining);
            if (result == timeout_error) {
                if (++timeouts_ >= MAX_TIMEOUTS)
                    close();
                break;
            }
            if (result <= 0 || static_cast<size_t>(result) > remaining) {
                close();
                break;
            }
            timeouts_ = 0;
            sent_ += static_cast<size_t>(result);
            if (sent_ != frame.size())
                break;
            buffered_bytes_ -= frame.size();
            frames_.pop_front();
            sent_ = 0;
        }
        // A failed queue submission may retry; an accepted close is never queued
        // twice. Keep both manager/context references until IDF frees its context.
        if (closing_ && !close_queued_)
            close_queued_ = close_socket(*this);
    }
};

struct SseContext {
    explicit SseContext(std::shared_ptr<SseSession> session) : session(std::move(session)) {}
    ~SseContext() { session->released_.store(true); }
    std::shared_ptr<SseSession> session;
};

class SseSessions {
 public:
    static constexpr size_t CAPACITY = 3;
    std::shared_ptr<SseSession> accept(uintptr_t server, int fd) {
        std::lock_guard<std::mutex> lock(mutex_);
        collect();
        if (sessions_.size() == CAPACITY)
            return {};
        auto session = std::make_shared<SseSession>(server, fd);
        sessions_.push_back(session);
        return session;
    }
    template <typename Visit>
    void visit(Visit visit) {
        std::lock_guard<std::mutex> lock(mutex_);
        collect();
        for (auto &session : sessions_)
            visit(*session);
    }
    void broadcast(const char *event, const std::string &data) {
        visit([&](SseSession &session) { session.send(event, data); });
    }
    void reset_sync() {
        visit([](SseSession &session) { session.restart_sync(); });
    }
    // Queue submission may fail when HTTPD is stopping or its mailbox is full.
    // No allocated work argument is transferred; caller retries on a later turn.
    template <typename Queue>
    void schedule(uintptr_t server, Queue queue) {
        if (!server) {
            work_queued_.store(false);
            return;
        }
        if (!work_queued_.exchange(true) && !queue(server))
            work_queued_.store(false);
    }
    void finish_work() { work_queued_.store(false); }
    template <typename Send, typename Close>
    void drain(Send send, Close close, int timeout_error) {
        // Called by a queued HTTPD work callback, never the main-loop producer.
        visit([&](SseSession &session) { session.drain(send, close, timeout_error); });
    }

 private:
    void collect() {
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if ((*it)->released())
                it = sessions_.erase(it);
            else
                ++it;
        }
    }
    std::mutex mutex_;
    std::atomic<bool> work_queued_{false};
    std::vector<std::shared_ptr<SseSession>> sessions_;
};

}  // namespace avionmesh
