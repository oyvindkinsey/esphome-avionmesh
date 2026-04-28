#include "avionmesh_web.h"
#include "avionmesh_hub.h"
#include "web_content.h"

#include "esphome/core/log.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/web_server_base/web_server_base.h"

#include <esp_http_server.h>
#include <sys/socket.h>
#include <cstring>
#include <cerrno>

static const char *TAG = "avionmesh.web";

namespace {
// Validate passphrase - accepts both base64 (new format) and hex strings (old format)
// Returns decoded length if valid, -1 if invalid
static int validate_passphrase(const std::string &s) {
    if (s.empty())
        return -1;

    // Minimum 8 characters
    if (s.length() < 8)
        return -1;

    // Check if it looks like base64 (multiple of 4, valid chars)
    if ((s.length() & 3) == 0) {
        size_t padding = 0;
        bool valid_base64 = true;
        for (size_t i = 0; i < s.length(); i++) {
            char c = s[i];
            if (c >= 'A' && c <= 'Z') continue;
            if (c >= 'a' && c <= 'z') continue;
            if (c >= '0' && c <= '9') continue;
            if (c == '+' || c == '/') continue;
            if (c == '=') {
                padding++;
                // Padding only allowed at end
                if (i != s.length() - padding) {
                    valid_base64 = false;
                    break;
                }
                continue;
            }
            valid_base64 = false;
            break;
        }

        if (valid_base64 && padding <= 2) {
            // Valid base64 - return decoded length
            return static_cast<int>((s.length() * 3) / 4 - padding);
        }
    }

    // Not base64 - accept as-is (old hex format or any other string)
    // Return length as decoded length for non-base64 strings
    return static_cast<int>(s.length());
}
}  // namespace

namespace avionmesh {

// ---------------------------------------------------------------------------
// SseSession — buffered SSE connection (modeled on ESPHome AsyncEventSourceResponse)
// ---------------------------------------------------------------------------

SseSession::SseSession(httpd_handle_t hd, int fd) : hd_(hd), fd_(fd) {}

void SseSession::destroy(void *ptr) {
    auto *ses = static_cast<SseSession *>(ptr);
    ses->fd_.store(0);
}

int SseSession::nonblocking_send(httpd_handle_t /*hd*/, int sockfd, const char *buf,
                                 size_t buf_len, int /*flags*/) {
    if (buf == nullptr)
        return HTTPD_SOCK_ERR_INVALID;
    int ret = ::send(sockfd, buf, buf_len, MSG_DONTWAIT);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return HTTPD_SOCK_ERR_TIMEOUT;
        return HTTPD_SOCK_ERR_FAIL;
    }
    return ret;
}

bool SseSession::send(const char *event, const std::string &data) {
    if (fd_.load() == 0)
        return false;

    // Drain any pending buffer first
    loop();
    if (!send_buf_.empty())
        return false;  // previous send still pending

    // Build chunked SSE frame (matches ESPHome AsyncEventSourceResponse layout)
    //
    // HTTP chunked transfer encoding:
    //   <hex-length>\r\n
    //   <SSE payload>
    //   \r\n
    //
    // Reserve 8 chars for hex length, filled in after payload is built.
    static constexpr size_t HEADER_LEN = 10;  // "        \r\n"
    send_buf_.reserve(data.size() + 64);
    send_buf_.assign("        \r\n");

    if (event && *event) {
        send_buf_ += "event: ";
        send_buf_ += event;
        send_buf_ += "\r\n";
    }
    send_buf_ += "data: ";
    send_buf_ += data;
    send_buf_ += "\r\n\r\n";   // data CRLF + blank-line terminator

    send_buf_ += "\r\n";       // chunk-terminating CRLF

    // Backfill hex chunk length (payload between header CRLF and trailing CRLF)
    int chunk_len = static_cast<int>(send_buf_.size()) - HEADER_LEN - 2;
    char len_str[9];
    snprintf(len_str, sizeof(len_str), "%08x", chunk_len);
    memcpy(send_buf_.data(), len_str, 8);

    bytes_sent_ = 0;
    loop();
    return true;
}

void SseSession::loop() {
    if (send_buf_.empty() || fd_.load() == 0)
        return;

    if (bytes_sent_ == send_buf_.size()) {
        send_buf_.clear();
        bytes_sent_ = 0;
        return;
    }

    size_t remaining = send_buf_.size() - bytes_sent_;
    int ret = httpd_socket_send(hd_, fd_.load(),
                                send_buf_.c_str() + bytes_sent_, remaining, 0);
    if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        if (++consecutive_failures_ >= MAX_FAILURES) {
            ESP_LOGW(TAG, "SSE session stuck, closing");
            fd_.store(0);
            send_buf_.clear();
        }
        return;
    }
    if (ret <= 0)
        return;

    consecutive_failures_ = 0;
    bytes_sent_ += ret;

    if (bytes_sent_ == send_buf_.size()) {
        send_buf_.clear();
        bytes_sent_ = 0;
    }
}

// ---------------------------------------------------------------------------
// AvionMeshWebHandler — SSE broadcast / sync
// ---------------------------------------------------------------------------

void AvionMeshWebHandler::send_event(const char *event, const std::string &data) {
    for (auto *ses : sse_sessions_) {
        ses->send(event, data);
    }
}

void AvionMeshWebHandler::send_initial_sync(SseSession *session) {
    auto &db = hub_->db_;
    auto &states = hub_->device_states_;

    // Meta event
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "{\"ble_state\":%u,\"mesh_initialized\":%s,\"rx_count\":%u}",
                 static_cast<uint8_t>(hub_->ble_state_),
                 hub_->mesh_initialized_ ? "true" : "false",
                 hub_->rx_count_);
        session->send("meta", buf);
        if (session->dead()) return;
    }

    // Devices in batches of 5
    {
        auto &devs = db.devices();
        static constexpr size_t BATCH = 5;
        for (size_t i = 0; i < devs.size(); i += BATCH) {
            std::string json = "{\"devices\":[";
            size_t end = std::min(i + BATCH, devs.size());
            for (size_t j = i; j < end; j++) {
                auto &dev = devs[j];
                if (j > i) json += ",";
                json += "{\"avion_id\":";
                json += std::to_string(dev.avion_id);
                json += ",\"name\":\"";
                json += dev.name;
                json += "\",\"product_type\":";
                json += std::to_string(dev.product_type);
                json += ",\"product_name\":\"";
                json += product_name(dev.product_type);
                json += "\",\"groups\":[";
                for (size_t g = 0; g < dev.groups.size(); g++) {
                    if (g > 0) json += ",";
                    json += std::to_string(dev.groups[g]);
                }
                json += "],\"mqtt_exposed\":";
                json += dev.mqtt_exposed ? "true" : "false";
                json += ",\"has_dimming\":";
                json += has_dimming(dev.product_type) ? "true" : "false";
                json += ",\"has_color_temp\":";
                json += has_color_temp(dev.product_type) ? "true" : "false";

                auto sit = states.find(dev.avion_id);
                if (sit != states.end() && sit->second.brightness_known) {
                    json += ",\"brightness\":";
                    json += std::to_string(sit->second.brightness);
                    if (sit->second.color_temp_known) {
                        json += ",\"color_temp\":";
                        json += std::to_string(sit->second.color_temp);
                    }
                }
                json += "}";
            }
            json += "]}";
            session->send("devices", json);
            if (session->dead()) return;
        }
    }

    // Groups in batches of 5
    {
        auto &grps = db.groups();
        static constexpr size_t BATCH = 5;
        for (size_t i = 0; i < grps.size(); i += BATCH) {
            std::string json = "{\"groups\":[";
            size_t end = std::min(i + BATCH, grps.size());
            for (size_t j = i; j < end; j++) {
                auto &grp = grps[j];
                if (j > i) json += ",";
                json += "{\"group_id\":";
                json += std::to_string(grp.group_id);
                json += ",\"name\":\"";
                json += grp.name;
                json += "\",\"members\":[";
                for (size_t m = 0; m < grp.member_ids.size(); m++) {
                    if (m > 0) json += ",";
                    json += std::to_string(grp.member_ids[m]);
                }
                json += "],\"mqtt_exposed\":";
                json += grp.mqtt_exposed ? "true" : "false";
                json += "}";
            }
            json += "]}";
            session->send("groups", json);
            if (session->dead()) return;
        }
    }

    // Mesh broadcast entity status
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"mesh_mqtt_exposed\":%s}",
                 hub_->mesh_mqtt_exposed_ ? "true" : "false");
        session->send("mesh_status", buf);
        if (session->dead()) return;
    }

    session->send("sync_complete", "{}");
    session->sync_pending = false;
}

void AvionMeshWebHandler::reset_sync() {
    for (auto *ses : sse_sessions_)
        ses->sync_pending = true;
}

void AvionMeshWebHandler::sse_loop() {
    bool did_sync = false;

    // Drain buffers, remove dead sessions, sync new ones
    for (size_t i = 0; i < sse_sessions_.size();) {
        auto *ses = sse_sessions_[i];
        ses->loop();
        if (ses->dead()) {
            ESP_LOGD(TAG, "Removing dead SSE session");
            delete ses;
            sse_sessions_[i] = sse_sessions_.back();
            sse_sessions_.pop_back();
            continue;
        }
        if (ses->sync_pending) {
            send_initial_sync(ses);
            did_sync = true;
        }
        ++i;
    }

    if (did_sync && hub_->mesh_initialized_) {
        uint32_t now = esphome::millis();
        if (now - last_state_read_ms_ > 10000) {
            last_state_read_ms_ = now;
            hub_->read_all_dimming();
            hub_->set_timeout("sse_color_read", 1000, [this]() {
                hub_->read_all_color();
            });
        }
    }
}

bool AvionMeshWebHandler::canHandle(AsyncWebServerRequest *request) const {
    std::string url = request->url();
    return url == "/ui" || url.rfind("/api/", 0) == 0;
}

void AvionMeshWebHandler::handleRequest(AsyncWebServerRequest *request) {
    std::string url = request->url();
    auto method = request->method();

    ESP_LOGD(TAG, "Request: %s %s", method == HTTP_POST ? "POST" : "GET", url.c_str());

    if (url == "/ui") {
        handle_index(request);
    } else if (url == "/api/events" && method == HTTP_GET) {
        handle_events(request);
    } else if (url == "/api/discover_mesh" && method == HTTP_POST) {
        handle_discover_mesh_post(request);
    } else if (url == "/api/scan_unassociated" && method == HTTP_POST) {
        handle_scan_unassociated_post(request);
    } else if (url == "/api/claim_device" && method == HTTP_POST) {
        handle_claim_device(request);
    } else if (url == "/api/add_discovered" && method == HTTP_POST) {
        handle_add_discovered(request);
    } else if (url == "/api/unclaim_device" && method == HTTP_POST) {
        handle_unclaim_device(request);
    } else if (url == "/api/examine_device" && method == HTTP_POST) {
        handle_examine_device_post(request);
    } else if (url == "/api/control" && method == HTTP_POST) {
        handle_control(request);
    } else if (url == "/api/create_group" && method == HTTP_POST) {
        handle_create_group(request);
    } else if (url == "/api/delete_group" && method == HTTP_POST) {
        handle_delete_group(request);
    } else if (url == "/api/add_to_group" && method == HTTP_POST) {
        handle_add_to_group(request);
    } else if (url == "/api/remove_from_group" && method == HTTP_POST) {
        handle_remove_from_group(request);
    } else if (url == "/api/import" && method == HTTP_POST) {
        handle_import(request);
    } else if (url == "/api/set_mqtt_exposed" && method == HTTP_POST) {
        handle_set_mqtt_exposed(request);
    } else if (url == "/api/save" && method == HTTP_POST) {
        handle_save(request);
    } else if (url == "/api/set_passphrase" && method == HTTP_POST) {
        handle_set_passphrase(request);
    } else if (url == "/api/generate_passphrase" && method == HTTP_POST) {
        handle_generate_passphrase(request);
    } else if (url == "/api/factory_reset" && method == HTTP_POST) {
        handle_factory_reset(request);
    } else {
        send_error(request, 404, "not_found");
    }
}

std::string AvionMeshWebHandler::read_body(AsyncWebServerRequest *request) {
    httpd_req_t *req = *request;
    size_t len = req->content_len;
    ESP_LOGI(TAG, "read_body: content_len=%zu", len);

    if (len == 0 || len > 16384) {  // 16KB limit for import requests
        ESP_LOGW(TAG, "read_body: invalid len=%zu", len);
        return {};
    }

    std::string body;
    body.resize(len);
    size_t total_read = 0;

    // Loop to read all data - ESPhttpd may not buffer everything at once
    while (total_read < len) {
        int ret = httpd_req_recv(req, &body[total_read], len - total_read);
        ESP_LOGD(TAG, "read_body loop: recv=%d total=%zu", ret, total_read);
        if (ret <= 0) {
            ESP_LOGW(TAG, "read_body: recv error (ret=%d after %zu/%zu bytes)", ret, total_read, len);
            return {};
        }
        total_read += ret;
    }

    body.resize(total_read);
    ESP_LOGI(TAG, "read_body: returning %zu bytes", total_read);
    return body;
}

void AvionMeshWebHandler::send_json(AsyncWebServerRequest *request, int code,
                                     const std::string &json) {
    auto *response = request->beginResponse(code, "application/json", json);
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
}

void AvionMeshWebHandler::send_error(AsyncWebServerRequest *request, int code,
                                      const char *message) {
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", message);
    send_json(request, code, buf);
}

void AvionMeshWebHandler::handle_index(AsyncWebServerRequest *request) {
    auto *response = request->beginResponse(200, "text/html",
                                             AVIONMESH_WEB_HTML, AVIONMESH_WEB_HTML_SIZE);
    response->addHeader("Content-Encoding", "gzip");
    response->addHeader("Cache-Control", "public, max-age=3600");
    request->send(response);
}

void AvionMeshWebHandler::handle_events(AsyncWebServerRequest *request) {
    httpd_req_t *req = *request;

    // Evict oldest session if at capacity
    while (sse_sessions_.size() >= MAX_SSE_SESSIONS) {
        auto *old = sse_sessions_.front();
        delete old;
        sse_sessions_.erase(sse_sessions_.begin());
    }

    httpd_resp_set_status(req, HTTPD_200);
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");

    httpd_resp_send_chunk(req, "\r\n", 2);

    int fd = httpd_req_to_sockfd(req);
    auto *ses = new SseSession(req->handle, fd);
    req->sess_ctx = ses;
    req->free_ctx = SseSession::destroy;

    httpd_sess_set_send_override(req->handle, fd, SseSession::nonblocking_send);

    sse_sessions_.push_back(ses);
}

void AvionMeshWebHandler::handle_discover_mesh_post(AsyncWebServerRequest *request) {
    ESP_LOGI(TAG, "discover_mesh requested, ble_state=%u, mesh_init=%d",
             static_cast<uint8_t>(hub_->ble_state_), hub_->mesh_initialized_);

    if (!hub_->mesh_initialized_) {
        ESP_LOGW(TAG, "discover_mesh failed: mesh not initialized (no passphrase?)");
        send_error(request, 503, "mesh_not_initialized");
        return;
    }

    if (hub_->ble_state_ != BleState::Ready) {
        ESP_LOGW(TAG, "discover_mesh failed: BLE not ready (state=%u)", static_cast<uint8_t>(hub_->ble_state_));
        send_error(request, 503, "ble_not_ready");
        return;
    }

    if (hub_->discovering_mesh_) {
        ESP_LOGW(TAG, "discover_mesh failed: already discovering");
        send_error(request, 409, "busy");
        return;
    }

    hub_->pending_discover_mesh_ = true;
    ESP_LOGI(TAG, "discover_mesh queued");
    send_json(request, 200, "{\"status\":\"started\"}");
}

void AvionMeshWebHandler::handle_scan_unassociated_post(AsyncWebServerRequest *request) {
    ESP_LOGI(TAG, "scan_unassociated requested, ble_state=%u, mesh_init=%d",
             static_cast<uint8_t>(hub_->ble_state_), hub_->mesh_initialized_);

    if (!hub_->mesh_initialized_) {
        ESP_LOGW(TAG, "scan_unassociated failed: mesh not initialized (no passphrase?)");
        send_error(request, 503, "mesh_not_initialized");
        return;
    }

    if (hub_->ble_state_ != BleState::Ready) {
        ESP_LOGW(TAG, "scan_unassociated failed: BLE not ready (state=%u)", static_cast<uint8_t>(hub_->ble_state_));
        send_error(request, 503, "ble_not_ready");
        return;
    }

    if (hub_->scanning_unassociated_) {
        ESP_LOGW(TAG, "scan_unassociated failed: already scanning");
        send_error(request, 409, "busy");
        return;
    }

    hub_->pending_scan_unassoc_ = true;
    ESP_LOGI(TAG, "scan_unassociated queued");
    send_json(request, 200, "{\"status\":\"started\"}");
}

void AvionMeshWebHandler::handle_claim_device(AsyncWebServerRequest *request) {
    if (hub_->ble_state_ != BleState::Ready) {
        send_error(request, 503, "ble_not_ready");
        return;
    }

    std::string body = read_body(request);
    if (body.empty()) {
        send_error(request, 400, "empty_body");
        return;
    }

    uint32_t uuid_hash = 0;
    std::string name;
    uint8_t product_type = 0;

    esphome::json::parse_json(body, [&](JsonObject root) -> bool {
        uuid_hash = root["uuid_hash"] | 0u;
        name = root["name"] | "Unknown";
        product_type = root["product_type"] | 0u;
        return true;
    });

    hub_->pending_claim_uuid_hash_ = uuid_hash;
    hub_->pending_claim_name_ = name;
    hub_->pending_claim_product_type_ = product_type;
    hub_->pending_claim_auto_ = true;
    send_json(request, 200, "{\"status\":\"started\"}");
}

void AvionMeshWebHandler::handle_add_discovered(AsyncWebServerRequest *request) {
    if (hub_->ble_state_ != BleState::Ready) {
        send_error(request, 503, "ble_not_ready");
        return;
    }

    std::string body = read_body(request);
    if (body.empty()) {
        send_error(request, 400, "empty_body");
        return;
    }

    uint16_t device_id = 0;
    std::string name;
    uint8_t product_type = 0;

    esphome::json::parse_json(body, [&](JsonObject root) -> bool {
        device_id = root["device_id"] | 0u;
        name = root["name"] | "Unknown";
        product_type = root["product_type"] | 0u;
        return true;
    });

    if (device_id == 0) {
        send_error(request, 400, "missing_device_id");
        return;
    }

    DeferredAction act;
    act.type = DeferredAction::AddDiscovered;
    act.id1 = device_id;
    act.name = std::move(name);
    act.product_type = product_type;
    {
        std::lock_guard<std::mutex> lock(hub_->action_mutex_);
        hub_->pending_actions_.push_back(std::move(act));
    }
    send_json(request, 200, "{\"status\":\"ok\"}");
}

void AvionMeshWebHandler::handle_unclaim_device(AsyncWebServerRequest *request) {
    std::string body = read_body(request);
    if (body.empty()) {
        send_error(request, 400, "empty_body");
        return;
    }

    uint16_t avion_id = 0;
    esphome::json::parse_json(body, [&](JsonObject root) -> bool {
        avion_id = root["avion_id"] | 0u;
        return true;
    });

    if (avion_id == 0) {
        send_error(request, 400, "missing_avion_id");
        return;
    }

    DeferredAction act;
    act.type = DeferredAction::UnclaimDevice;
    act.id1 = avion_id;
    {
        std::lock_guard<std::mutex> lock(hub_->action_mutex_);
        hub_->pending_actions_.push_back(std::move(act));
    }
    send_json(request, 200, "{\"status\":\"ok\"}");
}

void AvionMeshWebHandler::handle_examine_device_post(AsyncWebServerRequest *request) {
    if (hub_->ble_state_ != BleState::Ready) {
        send_error(request, 503, "ble_not_ready");
        return;
    }

    std::string body = read_body(request);
    if (body.empty()) {
        send_error(request, 400, "empty_body");
        return;
    }

    uint16_t avion_id = 0;
    esphome::json::parse_json(body, [&](JsonObject root) -> bool {
        avion_id = root["avion_id"] | 0u;
        return true;
    });

    if (avion_id == 0) {
        send_error(request, 400, "missing_avion_id");
        return;
    }

    hub_->pending_examine_id_ = avion_id;
    hub_->pending_examine_ = true;
    send_json(request, 200, "{\"status\":\"started\"}");
}

void AvionMeshWebHandler::handle_control(AsyncWebServerRequest *request) {
    if (hub_->ble_state_ != BleState::Ready) {
        send_error(request, 503, "ble_not_ready");
        return;
    }

    std::string body = read_body(request);
    if (body.empty()) {
        send_error(request, 400, "empty_body");
        return;
    }

    DeferredAction act;
    act.type = DeferredAction::Control;

    esphome::json::parse_json(body, [&](JsonObject root) -> bool {
        act.id1 = root["avion_id"] | 0u;
        if (root["brightness"].is<int>())
            act.brightness = root["brightness"] | 0;
        if (root["color_temp"].is<int>())
            act.color_temp = root["color_temp"] | 0;
        return true;
    });

    {
        std::lock_guard<std::mutex> lock(hub_->action_mutex_);
        hub_->pending_actions_.push_back(std::move(act));
    }
    send_json(request, 200, "{\"status\":\"ok\"}");
}

void AvionMeshWebHandler::handle_create_group(AsyncWebServerRequest *request) {
    std::string body = read_body(request);
    if (body.empty()) {
        send_error(request, 400, "empty_body");
        return;
    }

    std::string name;

    esphome::json::parse_json(body, [&](JsonObject root) -> bool {
        name = root["name"] | "Group";
        return true;
    });

    DeferredAction act;
    act.type = DeferredAction::CreateGroup;
    act.name = std::move(name);
    {
        std::lock_guard<std::mutex> lock(hub_->action_mutex_);
        hub_->pending_actions_.push_back(std::move(act));
    }
    send_json(request, 200, "{\"status\":\"ok\"}");
}

void AvionMeshWebHandler::handle_delete_group(AsyncWebServerRequest *request) {
    std::string body = read_body(request);
    if (body.empty()) {
        send_error(request, 400, "empty_body");
        return;
    }

    uint16_t group_id = 0;
    esphome::json::parse_json(body, [&](JsonObject root) -> bool {
        group_id = root["group_id"] | 0u;
        return true;
    });

    if (group_id == 0) {
        send_error(request, 400, "missing_group_id");
        return;
    }

    DeferredAction act;
    act.type = DeferredAction::DeleteGroup;
    act.id1 = group_id;
    {
        std::lock_guard<std::mutex> lock(hub_->action_mutex_);
        hub_->pending_actions_.push_back(std::move(act));
    }
    send_json(request, 200, "{\"status\":\"ok\"}");
}

void AvionMeshWebHandler::handle_add_to_group(AsyncWebServerRequest *request) {
    std::string body = read_body(request);
    if (body.empty()) {
        send_error(request, 400, "empty_body");
        return;
    }

    uint16_t avion_id = 0;
    uint16_t group_id = 0;

    esphome::json::parse_json(body, [&](JsonObject root) -> bool {
        avion_id = root["avion_id"] | 0u;
        group_id = root["group_id"] | 0u;
        return true;
    });

    if (avion_id == 0 || group_id == 0) {
        send_error(request, 400, "missing_ids");
        return;
    }

    DeferredAction act;
    act.type = DeferredAction::AddToGroup;
    act.id1 = avion_id;
    act.id2 = group_id;
    {
        std::lock_guard<std::mutex> lock(hub_->action_mutex_);
        hub_->pending_actions_.push_back(std::move(act));
    }
    send_json(request, 200, "{\"status\":\"ok\"}");
}

void AvionMeshWebHandler::handle_remove_from_group(AsyncWebServerRequest *request) {
    std::string body = read_body(request);
    if (body.empty()) {
        send_error(request, 400, "empty_body");
        return;
    }

    uint16_t avion_id = 0;
    uint16_t group_id = 0;

    esphome::json::parse_json(body, [&](JsonObject root) -> bool {
        avion_id = root["avion_id"] | 0u;
        group_id = root["group_id"] | 0u;
        return true;
    });

    if (avion_id == 0 || group_id == 0) {
        send_error(request, 400, "missing_ids");
        return;
    }

    DeferredAction act;
    act.type = DeferredAction::RemoveFromGroup;
    act.id1 = avion_id;
    act.id2 = group_id;
    {
        std::lock_guard<std::mutex> lock(hub_->action_mutex_);
        hub_->pending_actions_.push_back(std::move(act));
    }
    send_json(request, 200, "{\"status\":\"ok\"}");
}

void AvionMeshWebHandler::handle_import(AsyncWebServerRequest *request) {
    httpd_req_t *req = *request;
    ESP_LOGI(TAG, "handle_import: content_len=%d", req->content_len);
    std::string body = read_body(request);
    ESP_LOGI(TAG, "handle_import: body_len=%zu", body.size());
    if (body.empty()) {
        ESP_LOGW(TAG, "handle_import: empty body");
        send_error(request, 400, "empty_body");
        return;
    }

    DeferredAction act;
    act.type = DeferredAction::Import;
    act.body = std::move(body);
    {
        std::lock_guard<std::mutex> lock(hub_->action_mutex_);
        hub_->pending_actions_.push_back(std::move(act));
    }
    send_json(request, 200, "{\"status\":\"started\"}");
}

void AvionMeshWebHandler::handle_save(AsyncWebServerRequest *request) {
    DeferredAction act;
    act.type = DeferredAction::SaveDb;
    {
        std::lock_guard<std::mutex> lock(hub_->action_mutex_);
        hub_->pending_actions_.push_back(std::move(act));
    }
    send_json(request, 200, "{\"status\":\"ok\"}");
}

void AvionMeshWebHandler::handle_set_mqtt_exposed(AsyncWebServerRequest *request) {
    std::string body = read_body(request);
    if (body.empty()) {
        send_error(request, 400, "empty_body");
        return;
    }

    uint16_t id = 0;
    bool exposed = false;

    esphome::json::parse_json(body, [&](JsonObject root) -> bool {
        id = root["id"] | 0u;
        exposed = root["exposed"] | false;
        return true;
    });

    DeferredAction act;
    act.type = DeferredAction::SetMqttExposed;
    act.id1 = id;
    act.id2 = exposed ? 1 : 0;
    {
        std::lock_guard<std::mutex> lock(hub_->action_mutex_);
        hub_->pending_actions_.push_back(std::move(act));
    }
    send_json(request, 200, "{\"status\":\"ok\"}");
}

void AvionMeshWebHandler::handle_set_passphrase(AsyncWebServerRequest *request) {
    std::string body = read_body(request);
    if (body.empty()) {
        send_error(request, 400, "empty_body");
        return;
    }

    std::string passphrase;

    esphome::json::parse_json(body, [&](JsonObject root) -> bool {
        passphrase = root["passphrase"] | "";
        return true;
    });

    // Validate passphrase (accepts both base64 and old hex format)
    int decoded_len = validate_passphrase(passphrase);
    if (decoded_len < 8) {
        send_error(request, 400, "invalid_passphrase");
        ESP_LOGW(TAG, "Invalid passphrase: too short (len=%zu)", passphrase.size());
        return;
    }

    hub_->handle_set_passphrase(passphrase);
    send_json(request, 200, "{\"status\":\"ok\"}");
}

void AvionMeshWebHandler::handle_generate_passphrase(AsyncWebServerRequest *request) {
    hub_->handle_generate_passphrase();
    const std::string &passphrase = hub_->db_.passphrase();
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"passphrase\":\"%s\"}", passphrase.c_str());
    send_json(request, 200, buf);
}

void AvionMeshWebHandler::handle_factory_reset(AsyncWebServerRequest *request) {
    hub_->handle_factory_reset();
    send_json(request, 200, "{\"status\":\"ok\"}");
}

}  // namespace avionmesh
