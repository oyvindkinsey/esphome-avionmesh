#include "avionmesh_web.h"
#include "avionmesh_hub.h"
#include "web_content.h"
#include "web_style.h"
#include "web_script.h"

#include "esphome/core/log.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/web_server_base/web_server_base.h"

#include <esp_http_server.h>
#include <sys/socket.h>
#include <cstring>

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

void SseSession::destroy(void *ptr) {
    auto *ses = static_cast<SseSession *>(ptr);
    ses->fd.store(0);  // Mark as dead for cleanup
}

void AvionMeshWebHandler::send_event(const char *event, const std::string &data) {
    std::string payload;
    payload.reserve(data.size() + 48);
    if (event && *event) {
        payload += "event: ";
        payload += event;
        payload += "\r\n";
    }
    payload += "data: ";
    payload += data;
    payload += "\r\n\r\n";

    // Wrap in HTTP/1.1 chunked encoding
    char header[16];
    snprintf(header, sizeof(header), "%08x\r\n", (unsigned) payload.size());

    std::string chunk;
    chunk.reserve(payload.size() + 16);
    chunk += header;
    chunk += payload;
    chunk += "\r\n";

    std::lock_guard<std::mutex> lock(sse_mutex_);
    for (auto *ses : sse_sessions_) {
        int fd = ses->fd.load();
        if (fd != 0) {
            int ret = httpd_socket_send(ses->hd, fd, chunk.c_str(), chunk.size(), MSG_DONTWAIT);
            if (ret < 0) {
                ESP_LOGW(TAG, "SSE send failed (fd=%d), closing session", fd);
                httpd_sess_trigger_close(ses->hd, fd);
                ses->fd.store(0);
            }
        }
    }
}

void AvionMeshWebHandler::send_event_to(SseSession *session, const char *event,
                                         const std::string &data) {
    int fd = session->fd.load();
    if (fd == 0)
        return;

    std::string payload;
    payload.reserve(data.size() + 48);
    if (event && *event) {
        payload += "event: ";
        payload += event;
        payload += "\r\n";
    }
    payload += "data: ";
    payload += data;
    payload += "\r\n\r\n";

    char header[16];
    snprintf(header, sizeof(header), "%08x\r\n", (unsigned) payload.size());

    std::string chunk;
    chunk.reserve(payload.size() + 16);
    chunk += header;
    chunk += payload;
    chunk += "\r\n";

    int ret = httpd_socket_send(session->hd, fd, chunk.c_str(), chunk.size(), MSG_DONTWAIT);
    if (ret < 0) {
        ESP_LOGW(TAG, "SSE unicast failed (fd=%d), closing session", fd);
        httpd_sess_trigger_close(session->hd, fd);
        session->fd.store(0);
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
        send_event_to(session, "meta", buf);
        if (session->fd.load() == 0) return;
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
            send_event_to(session, "devices", json);
            if (session->fd.load() == 0) return;
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
            send_event_to(session, "groups", json);
            if (session->fd.load() == 0) return;
        }
    }

    // Mesh broadcast entity status
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"mesh_mqtt_exposed\":%s}",
                 hub_->mesh_mqtt_exposed_ ? "true" : "false");
        send_event_to(session, "mesh_status", buf);
        if (session->fd.load() == 0) return;
    }

    send_event_to(session, "sync_complete", "{}");
    session->sync_pending = false;
}

void AvionMeshWebHandler::reset_sync() {
    std::lock_guard<std::mutex> lock(sse_mutex_);
    for (auto *ses : sse_sessions_) {
        ses->sync_pending = true;
    }
}

void AvionMeshWebHandler::sse_loop() {
    // Collect pending sessions under lock, then sync outside lock
    // (send_initial_sync can block on socket writes)
    SseSession *pending[MAX_SSE_SESSIONS];
    size_t pending_count = 0;

    {
        std::lock_guard<std::mutex> lock(sse_mutex_);
        for (size_t i = 0; i < sse_sessions_.size();) {
            if (sse_sessions_[i]->fd.load() == 0) {
                ESP_LOGD(TAG, "Removing dead SSE session");
                delete sse_sessions_[i];
                sse_sessions_[i] = sse_sessions_.back();
                sse_sessions_.pop_back();
            } else {
                if (sse_sessions_[i]->sync_pending)
                    pending[pending_count++] = sse_sessions_[i];
                ++i;
            }
        }
    }

    bool did_sync = false;
    for (size_t i = 0; i < pending_count; i++) {
        if (pending[i]->fd.load() != 0) {
            send_initial_sync(pending[i]);
            did_sync = true;
        }
    }

    // Refresh mesh state on every new UI connection (debounced to 10 s)
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
    return url == "/ui" || url == "/ui.css" || url == "/ui.js" || url.rfind("/api/", 0) == 0;
}

void AvionMeshWebHandler::handleRequest(AsyncWebServerRequest *request) {
    std::string url = request->url();
    auto method = request->method();

    ESP_LOGD(TAG, "Request: %s %s", method == HTTP_POST ? "POST" : "GET", url.c_str());

    if (url == "/ui") {
        handle_index(request);
    } else if (url == "/ui.css") {
        handle_style(request);
    } else if (url == "/ui.js") {
        handle_script(request);
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
            if (total_read == 0) {
                ESP_LOGW(TAG, "read_body: no data available");
                return {};
            }
            break;
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

void AvionMeshWebHandler::handle_style(AsyncWebServerRequest *request) {
    auto *response = request->beginResponse(200, "text/css",
                                             AVIONMESH_WEB_STYLE, AVIONMESH_WEB_STYLE_SIZE);
    response->addHeader("Content-Encoding", "gzip");
    response->addHeader("Cache-Control", "public, max-age=3600");
    request->send(response);
}

void AvionMeshWebHandler::handle_script(AsyncWebServerRequest *request) {
    auto *response = request->beginResponse(200, "application/javascript",
                                             AVIONMESH_WEB_SCRIPT, AVIONMESH_WEB_SCRIPT_SIZE);
    response->addHeader("Content-Encoding", "gzip");
    response->addHeader("Cache-Control", "public, max-age=3600");
    request->send(response);
}

void AvionMeshWebHandler::handle_events(AsyncWebServerRequest *request) {
    httpd_req_t *req = *request;

    // Evict oldest sessions if at capacity
    {
        std::lock_guard<std::mutex> lock(sse_mutex_);
        while (sse_sessions_.size() >= MAX_SSE_SESSIONS) {
            auto *old = sse_sessions_.front();
            int fd = old->fd.load();
            if (fd != 0)
                httpd_sess_trigger_close(old->hd, fd);
            old->fd.store(0);
            // sse_loop() will clean up the dead entry
            sse_sessions_.erase(sse_sessions_.begin());
            delete old;
        }
    }

    httpd_resp_set_status(req, HTTPD_200);
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");

    httpd_resp_send_chunk(req, "\r\n", 2);

    auto *ses = new SseSession();
    req->sess_ctx = ses;
    req->free_ctx = SseSession::destroy;

    ses->hd = req->handle;
    ses->fd.store(httpd_req_to_sockfd(req));

    std::lock_guard<std::mutex> lock(sse_mutex_);
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
