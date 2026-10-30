#include "avionmesh_web.h"
#include "avionmesh_hub.h"
#include "sse_initial_sync.h"
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
            if (c >= 'A' && c <= 'Z')
                continue;
            if (c >= 'a' && c <= 'z')
                continue;
            if (c >= '0' && c <= '9')
                continue;
            if (c == '+' || c == '/')
                continue;
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

void AvionMeshWebHandler::sse_free_context(void *ptr) {
    delete static_cast<SseContext *>(ptr);
}

int AvionMeshWebHandler::nonblocking_send(httpd_handle_t, int fd, const char *buf, size_t len, int) {
    if (!buf)
        return HTTPD_SOCK_ERR_INVALID;
    const int result = ::send(fd, buf, len, MSG_DONTWAIT);
    if (result < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK ? HTTPD_SOCK_ERR_TIMEOUT : HTTPD_SOCK_ERR_FAIL;
    return result;
}

void AvionMeshWebHandler::sse_drain_work(void *arg) {
    auto *handler = static_cast<AvionMeshWebHandler *>(arg);
    handler->sse_sessions_.drain(
        [](const SseSession &session, const char *data, size_t length) {
            return httpd_socket_send(reinterpret_cast<httpd_handle_t>(session.server), session.fd,
                                     data, length, 0);
        },
        [](const SseSession &session) {
            return httpd_sess_trigger_close(reinterpret_cast<httpd_handle_t>(session.server),
                                             session.fd) == ESP_OK;
        }, HTTPD_SOCK_ERR_TIMEOUT);
    handler->sse_sessions_.finish_work();
}

void AvionMeshWebHandler::send_event(const char *event, const std::string &data) {
    sse_sessions_.broadcast(event, data);
}

void AvionMeshWebHandler::send_initial_sync(SseSession *session) {
    char meta[128];
    snprintf(meta, sizeof(meta), "{\"ble_state\":%u,\"mesh_initialized\":%s,\"rx_count\":%u}",
             static_cast<uint8_t>(hub_->ble_state_), hub_->mesh_initialized_ ? "true" : "false",
             static_cast<unsigned>(hub_->rx_count_));
    send_sse_initial_sync(session, hub_->db_, hub_->device_states_, meta, hub_->mesh_mqtt_exposed_,
        [this](uint16_t id) { return hub_->controller_bindings_json(id); },
        [this](uint16_t id) { return hub_->controller_input_status(id); });
}

void AvionMeshWebHandler::reset_sync() {
    sse_sessions_.reset_sync();
}

void AvionMeshWebHandler::sse_loop() {
    bool did_sync = false;
    httpd_handle_t server = nullptr;
    sse_sessions_.visit([&](SseSession &session) {
        server = reinterpret_cast<httpd_handle_t>(session.server);
        if (session.sync_pending) {
            send_initial_sync(&session);
            did_sync = did_sync || !session.sync_pending;
        }
    });
    // All socket I/O, session lookup and asynchronous close scheduling run on
    // HTTPD's task, serialized with its native socket/context cleanup.
    sse_sessions_.schedule(reinterpret_cast<uintptr_t>(server), [this](uintptr_t handle) {
        return httpd_queue_work(reinterpret_cast<httpd_handle_t>(handle), sse_drain_work, this) == ESP_OK;
    });
    if (did_sync && hub_->mesh_initialized_) {
        const uint32_t now = esphome::millis();
        if (now - last_state_read_ms_ > 10000) {
            last_state_read_ms_ = now;
            hub_->read_all_dimming();
            hub_->set_timeout("sse_color_read", 1000, [this]() { hub_->read_all_color(); });
        }
    }
}

bool AvionMeshWebHandler::canHandle(AsyncWebServerRequest *request) const {
    char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
    std::string url(request->url_to(url_buf));
    return url == "/ui" || url.rfind("/api/", 0) == 0;
}

void AvionMeshWebHandler::handleRequest(AsyncWebServerRequest *request) {
    char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
    std::string url(request->url_to(url_buf));
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
    } else if (url == "/api/controller_buttons/read" && method == HTTP_POST) {
        handle_controller_bindings_read(request);
    } else if (url == "/api/controller_buttons/setup" && method == HTTP_POST) {
        handle_controller_setup(request);
    } else if (url == "/api/controller_input_mode" && method == HTTP_POST) {
        handle_local_dimmer_mode(request);
    } else if (url == "/api/set_mqtt_exposed" && method == HTTP_POST) {
        handle_set_mqtt_exposed(request);
    } else if (url == "/api/rename_device" && method == HTTP_POST) {
        handle_rename_device(request);
    } else if (url == "/api/rename_group" && method == HTTP_POST) {
        handle_rename_group(request);
    } else if (url == "/api/save" && method == HTTP_POST) {
        handle_save(request);
    } else if (url == "/api/export" && method == HTTP_GET) {
        handle_export(request);
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

void AvionMeshWebHandler::handleBody(AsyncWebServerRequest * /*request*/, uint8_t *data, size_t len,
                                     size_t index, size_t total) {
    if (index == 0) {
        body_buf_.clear();
        body_received_ = true;
        body_too_big_ = total > 16384;
        if (!body_too_big_)
            body_buf_.reserve(total);
    }
    if (body_too_big_)
        return;
    body_buf_.append(reinterpret_cast<const char *>(data), len);
}

std::string AvionMeshWebHandler::read_body(AsyncWebServerRequest *request) {
    // Newer ESPHome web_server_idf consumes the body and hands it to handleBody().
    // Use the buffer whenever handleBody ran - even for an empty body, where
    // falling through to the httpd read would hit the already-consumed body.
    if (body_received_) {
        body_received_ = false;
        std::string body;
        body.swap(body_buf_);
        if (body_too_big_) {
            body_too_big_ = false;
            ESP_LOGW(TAG, "read_body: body exceeds 16KB limit");
            return {};
        }
        ESP_LOGI(TAG, "read_body: %zu bytes via handleBody", body.size());
        return body;
    }

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
            ESP_LOGW(TAG, "read_body: recv error (ret=%d after %zu/%zu bytes)", ret, total_read,
                     len);
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
    // ESPHome's wrapper maps unlisted status codes to 500. Set these new
    // endpoint statuses explicitly after it initializes the response.
    if (code == 202)
        httpd_resp_set_status(*request, "202 Accepted");
    if (code == 403)
        httpd_resp_set_status(*request, "403 Forbidden");
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

void AvionMeshWebHandler::send_error(AsyncWebServerRequest *request, int code,
                                     const char *message) {
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", message);
    send_json(request, code, buf);
}

void AvionMeshWebHandler::handle_index(AsyncWebServerRequest *request) {
    auto *response =
        request->beginResponse(200, "text/html", AVIONMESH_WEB_HTML, AVIONMESH_WEB_HTML_SIZE);
    response->addHeader("Content-Encoding", "gzip");
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
}

void AvionMeshWebHandler::handle_events(AsyncWebServerRequest *request) {
    httpd_req_t *req = *request;

    auto session = sse_sessions_.accept(reinterpret_cast<uintptr_t>(req->handle),
                                         httpd_req_to_sockfd(req));
    if (!session) {
        send_error(request, 503, "too_many_event_streams");
        return;
    }
    // IDF owns this independent shared reference until free_ctx runs. Manager
    // cleanup/overflow never deletes the context to which IDF still points.
    req->sess_ctx = new SseContext(session);
    req->free_ctx = sse_free_context;
    httpd_resp_set_status(req, HTTPD_200);
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    if (httpd_resp_send_chunk(req, "\r\n", 2) != ESP_OK) {
        sse_sessions_.visit([&](SseSession &entry) {
            if (&entry == session.get())
                entry.close();
        });
        return;
    }
    httpd_sess_set_send_override(req->handle, session->fd, nonblocking_send);
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
        ESP_LOGW(TAG, "discover_mesh failed: BLE not ready (state=%u)",
                 static_cast<uint8_t>(hub_->ble_state_));
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
        ESP_LOGW(TAG, "scan_unassociated failed: BLE not ready (state=%u)",
                 static_cast<uint8_t>(hub_->ble_state_));
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
    if (hub_->associating_ || hub_->discovering_mesh_ || hub_->pending_claim_auto_) {
        send_error(request, 409, "busy");
        return;
    }
    if (!hub_->crypto_initialized_ || hub_->db_.passphrase().empty()) {
        send_error(request, 503, "mesh_not_initialized");
        return;
    }
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

void AvionMeshWebHandler::handle_controller_bindings_read(AsyncWebServerRequest *request) {
    const auto body = read_body(request);
    JsonDocument doc;
    if (deserializeJson(doc, body) || !doc["controller_id"].is<unsigned int>() ||
        doc["controller_id"].as<unsigned int>() < 32896 ||
        doc["controller_id"].as<unsigned int>() > 65407) {
        send_error(request, 400, "invalid_controller_id");
        return;
    }
    DeferredAction act;
    act.type = DeferredAction::RefreshControllerBindings;
    act.id1 = doc["controller_id"].as<uint16_t>();
    {
        std::lock_guard<std::mutex> lock(hub_->action_mutex_);
        hub_->pending_actions_.push_back(std::move(act));
    }
    send_json(request, 202, "{\"status\":\"started\"}");
}

void AvionMeshWebHandler::handle_controller_setup(AsyncWebServerRequest *request) {
    const auto body = read_body(request);
    JsonDocument doc;
    if (deserializeJson(doc, body) || !doc["controller_id"].is<unsigned int>() ||
        doc["controller_id"].as<unsigned int>() < 32896 ||
        doc["controller_id"].as<unsigned int>() > 65407) {
        send_error(request, 400, "invalid_controller_id");
        return;
    }
    DeferredAction action;
    action.type = DeferredAction::SetupControllerButtons;
    action.id1 = doc["controller_id"].as<uint16_t>();
    {
        std::lock_guard<std::mutex> lock(hub_->action_mutex_);
        hub_->pending_actions_.push_back(std::move(action));
    }
    send_json(request, 202, "{\"status\":\"started\"}");
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

void AvionMeshWebHandler::handle_local_dimmer_mode(AsyncWebServerRequest *request) {
    JsonDocument doc;
    if (deserializeJson(doc, read_body(request)) || !doc["avion_id"].is<unsigned int>() ||
        !doc["avion_id"].as<unsigned int>() || doc["avion_id"].as<unsigned int>() > 65535 ||
        !doc["enabled"].is<bool>()) {
        send_error(request, 400, "invalid_controller_input_mode");
        return;
    }
    DeferredAction act;
    act.type = DeferredAction::SetLocalDimmerMode;
    act.id1 = doc["avion_id"].as<uint16_t>();
    act.id2 = doc["enabled"].as<bool>();
    {
        std::lock_guard<std::mutex> lock(hub_->action_mutex_);
        hub_->pending_actions_.push_back(std::move(act));
    }
    send_json(request, 200, "{\"status\":\"queued\"}");
}

namespace {
// Minimal JSON string escaping for device/group names.
void append_json_string(std::string &out, const std::string &s) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}
}  // namespace

void AvionMeshWebHandler::handle_export(AsyncWebServerRequest *request) {
    // The passphrase is always included: a backup without the key cannot
    // restore the mesh. Physical and network security of the backup file
    // and the device itself are the user's responsibility.
    std::string body = "{\"reset\":false,\"passphrase\":";
    append_json_string(body, hub_->db_.passphrase());
    body += ",\"devices\":[";
    bool first = true;
    for (const auto &dev : hub_->db_.devices()) {
        if (!first)
            body += ',';
        first = false;
        body += "{\"device_id\":" + std::to_string(dev.avion_id) + ",\"name\":";
        append_json_string(body, dev.name);
        body += ",\"product_type\":" + std::to_string(dev.product_type) + ",\"groups\":[";
        for (size_t i = 0; i < dev.groups.size(); i++) {
            if (i > 0)
                body += ',';
            body += std::to_string(dev.groups[i]);
        }
        body += "],\"mqtt_exposed\":" + std::string(dev.mqtt_exposed ? "true" : "false") + "}";
    }
    body += "],\"groups\":[";
    first = true;
    for (const auto &grp : hub_->db_.groups()) {
        if (!first)
            body += ',';
        first = false;
        body += "{\"group_id\":" + std::to_string(grp.group_id) + ",\"name\":";
        append_json_string(body, grp.name);
        body += ",\"members\":[";
        for (size_t i = 0; i < grp.member_ids.size(); i++) {
            if (i > 0)
                body += ',';
            body += std::to_string(grp.member_ids[i]);
        }
        body += "],\"mqtt_exposed\":" + std::string(grp.mqtt_exposed ? "true" : "false") + "}";
    }
    body += "]}";

    auto *response = request->beginResponse(200, "application/json", body);
    response->addHeader("Content-Disposition", "attachment; filename=\"avionmesh-backup.json\"");
    request->send(response);
}

void AvionMeshWebHandler::handle_rename_device(AsyncWebServerRequest *request) {
    std::string body = read_body(request);
    if (body.empty()) {
        send_error(request, 400, "empty_body");
        return;
    }

    uint16_t avion_id = 0;
    std::string name;
    esphome::json::parse_json(body, [&](JsonObject root) -> bool {
        avion_id = root["avion_id"] | 0u;
        name = root["name"] | "";
        return true;
    });
    if (avion_id == 0 || name.empty()) {
        send_error(request, 400, "invalid_name");
        return;
    }

    DeferredAction act;
    act.type = DeferredAction::RenameDevice;
    act.id1 = avion_id;
    act.name = name;
    {
        std::lock_guard<std::mutex> lock(hub_->action_mutex_);
        hub_->pending_actions_.push_back(std::move(act));
    }
    send_json(request, 200, "{\"status\":\"ok\"}");
}

void AvionMeshWebHandler::handle_rename_group(AsyncWebServerRequest *request) {
    std::string body = read_body(request);
    if (body.empty()) {
        send_error(request, 400, "empty_body");
        return;
    }

    uint16_t group_id = 0;
    std::string name;
    esphome::json::parse_json(body, [&](JsonObject root) -> bool {
        group_id = root["group_id"] | 0u;
        name = root["name"] | "";
        return true;
    });
    if (group_id == 0 || name.empty()) {
        send_error(request, 400, "invalid_name");
        return;
    }

    DeferredAction act;
    act.type = DeferredAction::RenameGroup;
    act.id1 = group_id;
    act.name = name;
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

    JsonDocument doc;
    if (deserializeJson(doc, body) || !doc["id"].is<unsigned int>() ||
        doc["id"].as<unsigned int>() > 65535 || !doc["exposed"].is<bool>()) {
        send_error(request, 400, "invalid_mqtt_exposure");
        return;
    }
    const uint16_t id = doc["id"].as<uint16_t>();
    const bool exposed = doc["exposed"].as<bool>();

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
