#include "avionmesh_web.h"
#include "avionmesh_hub.h"
#include "web_content.h"

#include "esphome/core/log.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/web_server_base/web_server_base.h"

#include <esp_http_server.h>
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
            int ret = httpd_socket_send(ses->hd, fd, chunk.c_str(), chunk.size(), 0);
            if (ret < 0) {
                ESP_LOGW(TAG, "SSE send failed (fd=%d, err=%d)", fd, ret);
            }
        }
    }
}

void AvionMeshWebHandler::sse_loop() {
    std::lock_guard<std::mutex> lock(sse_mutex_);
    for (size_t i = 0; i < sse_sessions_.size();) {
        if (sse_sessions_[i]->fd.load() == 0) {
            ESP_LOGD(TAG, "Removing dead SSE session");
            delete sse_sessions_[i];
            sse_sessions_[i] = sse_sessions_.back();
            sse_sessions_.pop_back();
        } else {
            ++i;
        }
    }
}

bool AvionMeshWebHandler::canHandle(AsyncWebServerRequest *request) const {
    std::string url = request->url();
    return url == "/avionmesh" || url.rfind("/avionmesh/", 0) == 0;
}

void AvionMeshWebHandler::handleRequest(AsyncWebServerRequest *request) {
    std::string url = request->url();
    auto method = request->method();

    ESP_LOGD(TAG, "Request: %s %s", method == HTTP_POST ? "POST" : "GET", url.c_str());

    if (url == "/avionmesh") {
        handle_index(request);
    } else if (url == "/avionmesh/api/status" && method == HTTP_GET) {
        handle_status(request);
    } else if (url == "/avionmesh/api/events" && method == HTTP_GET) {
        handle_events(request);
    } else if (url == "/avionmesh/api/discover_mesh" && method == HTTP_POST) {
        handle_discover_mesh_post(request);
    } else if (url == "/avionmesh/api/scan_unassociated" && method == HTTP_POST) {
        handle_scan_unassociated_post(request);
    } else if (url == "/avionmesh/api/claim_device" && method == HTTP_POST) {
        handle_claim_device(request);
    } else if (url == "/avionmesh/api/add_discovered" && method == HTTP_POST) {
        handle_add_discovered(request);
    } else if (url == "/avionmesh/api/unclaim_device" && method == HTTP_POST) {
        handle_unclaim_device(request);
    } else if (url == "/avionmesh/api/examine_device" && method == HTTP_POST) {
        handle_examine_device_post(request);
    } else if (url == "/avionmesh/api/control" && method == HTTP_POST) {
        handle_control(request);
    } else if (url == "/avionmesh/api/create_group" && method == HTTP_POST) {
        handle_create_group(request);
    } else if (url == "/avionmesh/api/delete_group" && method == HTTP_POST) {
        handle_delete_group(request);
    } else if (url == "/avionmesh/api/add_to_group" && method == HTTP_POST) {
        handle_add_to_group(request);
    } else if (url == "/avionmesh/api/remove_from_group" && method == HTTP_POST) {
        handle_remove_from_group(request);
    } else if (url == "/avionmesh/api/import" && method == HTTP_POST) {
        handle_import(request);
    } else if (url == "/avionmesh/api/cloud_import" && method == HTTP_POST) {
        handle_cloud_import_post(request);
    } else if (url == "/avionmesh/api/set_passphrase" && method == HTTP_POST) {
        handle_set_passphrase(request);
    } else if (url == "/avionmesh/api/generate_passphrase" && method == HTTP_POST) {
        handle_generate_passphrase(request);
    } else if (url == "/avionmesh/api/factory_reset" && method == HTTP_POST) {
        handle_factory_reset(request);
    } else {
        send_error(request, 404, "not_found");
    }
}

std::string AvionMeshWebHandler::read_body(AsyncWebServerRequest *request) {
    httpd_req_t *req = *request;
    size_t len = req->content_len;
    if (len == 0 || len > 4096)
        return {};

    std::string body;
    body.resize(len);
    int ret = httpd_req_recv(req, &body[0], len);
    if (ret <= 0)
        return {};

    body.resize(ret);
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

void AvionMeshWebHandler::handle_status(AsyncWebServerRequest *request) {
    auto &db = hub_->db_;
    auto &states = hub_->device_states_;

    std::string json = "{\"ble_state\":";
    json += std::to_string(static_cast<uint8_t>(hub_->ble_state_));
    json += ",\"mesh_initialized\":";
    json += hub_->mesh_initialized_ ? "true" : "false";
    json += ",\"rx_count\":";
    json += std::to_string(hub_->rx_count_);
    json += ",\"devices\":[";

    bool first = true;
    for (auto &dev : db.devices()) {
        if (!first) json += ",";
        first = false;
        json += "{\"avion_id\":";
        json += std::to_string(dev.avion_id);
        json += ",\"name\":\"";
        json += dev.name;
        json += "\",\"product_type\":";
        json += std::to_string(dev.product_type);
        json += ",\"product_name\":\"";
        json += product_name(dev.product_type);
        json += "\",\"groups\":[";
        for (size_t i = 0; i < dev.groups.size(); i++) {
            if (i > 0) json += ",";
            json += std::to_string(dev.groups[i]);
        }
        json += "]";

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

    json += "],\"groups\":[";

    first = true;
    for (auto &grp : db.groups()) {
        if (!first) json += ",";
        first = false;
        json += "{\"group_id\":";
        json += std::to_string(grp.group_id);
        json += ",\"name\":\"";
        json += grp.name;
        json += "\",\"members\":[";
        for (size_t i = 0; i < grp.member_ids.size(); i++) {
            if (i > 0) json += ",";
            json += std::to_string(grp.member_ids[i]);
        }
        json += "]}";
    }

    json += "]}";
    send_json(request, 200, json);
}

void AvionMeshWebHandler::handle_events(AsyncWebServerRequest *request) {
    httpd_req_t *req = *request;

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

    hub_->handle_add_discovered(device_id, name, product_type);
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

    hub_->handle_unclaim_device(avion_id);
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

    uint16_t avion_id = 0;
    int brightness = -1;
    int color_temp = -1;

    esphome::json::parse_json(body, [&](JsonObject root) -> bool {
        avion_id = root["avion_id"] | 0u;
        if (root["brightness"].is<int>())
            brightness = root["brightness"] | 0;
        if (root["color_temp"].is<int>())
            color_temp = root["color_temp"] | 0;
        return true;
    });

    if (brightness >= 0) {
        Command cmd;
        cmd_brightness(avion_id, static_cast<uint8_t>(brightness), cmd);
        send_cmd(hub_->mesh_ctx_, cmd);

        auto &state = hub_->device_states_[avion_id];
        state.brightness = static_cast<uint8_t>(brightness);
        state.brightness_known = true;
        hub_->publish_device_state(avion_id);
    }

    if (color_temp > 0) {
        Command cmd;
        cmd_color_temp(avion_id, static_cast<uint16_t>(color_temp), cmd);
        send_cmd(hub_->mesh_ctx_, cmd);

        auto &state = hub_->device_states_[avion_id];
        state.color_temp = static_cast<uint16_t>(color_temp);
        state.color_temp_known = true;
        hub_->publish_device_state(avion_id);
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

    uint16_t group_id = hub_->next_group_id();
    if (group_id == 0) {
        send_error(request, 503, "no_available_ids");
        return;
    }

    hub_->handle_create_group(group_id, name);
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"group_id\":%u}", group_id);
    send_json(request, 200, buf);
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

    hub_->handle_delete_group(group_id);
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

    hub_->handle_add_to_group(avion_id, group_id);
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

    hub_->handle_remove_from_group(avion_id, group_id);
    send_json(request, 200, "{\"status\":\"ok\"}");
}

void AvionMeshWebHandler::handle_import(AsyncWebServerRequest *request) {
    std::string body = read_body(request);
    if (body.empty()) {
        send_error(request, 400, "empty_body");
        return;
    }

    int added_devices = 0, added_groups = 0;

    esphome::json::parse_json(body, [&](JsonObject root) -> bool {
        JsonArray devices = root["devices"];
        for (JsonObject dev : devices) {
            uint16_t device_id = dev["device_id"] | 0u;
            std::string name = dev["name"] | "Unknown";
            uint8_t product_type = dev["product_type"] | 0u;
            if (device_id == 0) continue;
            if (hub_->db_.find_device(device_id)) continue;

            bool has_dim = has_dimming(product_type);
            bool has_ct = has_color_temp(product_type);
            hub_->db_.add_device(device_id, product_type, name);
            hub_->discovery_.publish_light(device_id, name, has_dim, has_ct,
                                           product_name(product_type));
            added_devices++;
        }

        JsonArray groups = root["groups"];
        for (JsonObject grp : groups) {
            uint16_t group_id = grp["group_id"] | 0u;
            std::string name = grp["name"] | "Group";
            if (group_id == 0) continue;
            if (!hub_->db_.find_group(group_id)) {
                hub_->db_.add_group(group_id, name);
                hub_->discovery_.publish_light(group_id, name, true, true);
                added_groups++;
            }

            JsonArray members = grp["members"];
            for (JsonVariant m : members) {
                uint16_t member_id = m.as<uint16_t>();
                if (member_id > 0) {
                    hub_->db_.add_device_to_group(member_id, group_id);
                    Command cmd;
                    cmd_insert_group(member_id, group_id, cmd);
                    send_cmd(hub_->mesh_ctx_, cmd);
                }
            }
        }

        return true;
    });

    hub_->publish_all_discovery();
    hub_->subscribe_all_commands();

    char buf[96];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"ok\",\"added_devices\":%d,\"added_groups\":%d}",
             added_devices, added_groups);
    send_json(request, 200, buf);
}

void AvionMeshWebHandler::handle_cloud_import_post(AsyncWebServerRequest *request) {
    std::string body = read_body(request);
    if (body.empty()) {
        send_error(request, 400, "empty_body");
        return;
    }

    std::string email, password;

    esphome::json::parse_json(body, [&](JsonObject root) -> bool {
        email = root["email"] | "";
        password = root["password"] | "";
        return true;
    });

    if (email.empty() || password.empty()) {
        send_error(request, 400, "missing_credentials");
        return;
    }

    // Perform cloud import - this may take several seconds
    std::string result = hub_->handle_cloud_import(email, password);

    // Send the result
    send_json(request, 200, result);
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
