#include "cloud_sync.h"
#include "cloud_http_bounds.h"

#ifdef USE_ESP32
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ctime>
#include "cloud_scale_test.h"
#include "esphome/core/log.h"

namespace avionmesh {
namespace {
void erase_secret(std::string &value) {
    volatile char *p = value.empty() ? nullptr : &value[0];
    for (size_t i = 0; i < value.size(); ++i)
        p[i] = 0;
    value.clear();
}
esp_err_t receive_http(esp_http_client_event_t *event) {
    auto &response = *static_cast<CloudHttpResponseBound *>(event->user_data);
    CloudHttpEvent kind = CloudHttpEvent::Terminal;
    switch (event->event_id) {
        case HTTP_EVENT_ON_CONNECTED:
            kind = CloudHttpEvent::Connected;
            break;
        case HTTP_EVENT_HEADERS_SENT:
            kind = CloudHttpEvent::HeadersSent;
            break;
        case HTTP_EVENT_ON_HEADER:
            kind = CloudHttpEvent::Header;
            break;
        case HTTP_EVENT_ON_DATA:
            kind = CloudHttpEvent::Data;
            break;
        default:
            break;
    }
    const bool data_event = kind == CloudHttpEvent::Data;
    if (data_event)
        cloud_scale_memory("receive_before", response.body.size());
    const bool accepted = handle_cloud_http_event(
        response, kind, static_cast<const char *>(event->data), event->data_len,
        esp_timer_get_time(),
        [event](int timeout_ms) { esp_http_client_set_timeout_ms(event->client, timeout_ms); },
        [event]() {
            // IDF ignores ON_DATA handler errors. Close the transport itself;
            // keep the client/parser alive until perform() unwinds and cleanup.
            esp_http_client_close(event->client);
        });
    if (data_event)
        cloud_scale_memory("receive_after", response.body.size());
    return accepted ? ESP_OK : ESP_FAIL;
}
bool fetch_cloud(const std::string &path, const std::string &token, const std::string *login,
                 JsonDocument &doc, std::string &error, int64_t deadline) {
    CloudHttpResponseBound response(deadline);
    const int initial_timeout_ms = response.io_timeout_ms(esp_timer_get_time());
    if (!initial_timeout_ms) {
        error = "sync_timeout";
        return false;
    }
    // Only fixed-host HTTPS; never follow a redirect with credentials or tokens.
#ifdef AVIONMESH_CLOUD_SCALE_TEST
    const std::string url = std::string(AVIONMESH_CLOUD_SCALE_URL) + path;
#else
    const std::string url = "https://api.avi-on.com/" + path;
#endif
    esp_http_client_config_t config{};
    config.url = url.c_str();
#ifdef AVIONMESH_CLOUD_SCALE_TEST
    config.cert_pem = AVIONMESH_CLOUD_SCALE_CA;
#else
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    config.disable_auto_redirect = true;
    config.max_authorization_retries = -1;  // Token auth only; no HTTP auth negotiation.
    config.timeout_ms = initial_timeout_ms;
    config.event_handler = receive_http;
    config.user_data = &response;
    cloud_scale_memory("https_before");
    auto client = esp_http_client_init(&config);
    cloud_scale_memory("https_initialized");
    if (!client) {
        error = "https_allocation_failed";
        return false;
    }
    esp_http_client_set_header(client, "Accept", "application/api.avi-on.v3");
    std::string authorization;
    if (!token.empty()) {
        authorization = "Token " + token;
        esp_http_client_set_header(client, "Authorization", authorization.c_str());
    }
    if (login) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, login->data(), login->size());
    }
    auto result = esp_http_client_perform(client);
    cloud_scale_memory("https_received", response.body.size());
    const int status = esp_http_client_get_status_code(client);
    const bool framing_complete = esp_http_client_is_complete_data_received(client);
    esp_http_client_cleanup(client);
    erase_secret(authorization);
    const bool complete =
        response.complete(result == ESP_OK, framing_complete, esp_timer_get_time());
    if (response.timed_out || response.too_big) {
        error = response.timed_out ? "sync_timeout" : "cloud_response_too_large";
        erase_secret(response.body);
        return false;
    }
    // ESP-IDF can return ESP_FAIL for a perfectly valid HTTP 401 when its
    // built-in HTTP auth negotiation is disabled. Classify the status first.
    if (status == 401 || status == 403 || (login && status == 400)) {
        error = "cloud_login_rejected";
        erase_secret(response.body);
        return false;
    }
    if (!complete) {
        ESP_LOGW("avionmesh.cloud", "HTTPS request failed (result=%s, status=%d)",
                 esp_err_to_name(result), status);
        error = "cloud_connection_failed";
        erase_secret(response.body);
        return false;
    }
    if (status != 200 && status != 201) {
        error = "cloud_http_" + std::to_string(status);
        erase_secret(response.body);
        return false;
    }
    doc.clear();
    cloud_scale_memory("parse_before", response.body.size());
    const auto parsed =
        deserializeJson(doc, response.body, DeserializationOption::NestingLimit(16));
    cloud_scale_memory("parse_body_overlap", response.body.size());
    erase_secret(response.body);
    cloud_scale_memory("parse_body_erased");
    if (parsed || !doc.is<JsonObject>()) {
        error = "invalid_cloud_response";
        return false;
    }
    return true;
}
}  // namespace

bool CloudSync::queue(std::string email, std::string password) {
    std::lock_guard<std::mutex> lock(mutex_);
#ifdef AVIONMESH_CLOUD_SCALE_TEST
    // No real login can ever leave a test build, including through the normal UI.
    if (email != "scale@example.invalid" || password != "synthetic-scale-only") {
        erase_secret(email);
        erase_secret(password);
        return false;
    }
#endif
    if (busy_) {
        erase_secret(password);
        return false;
    }
    busy_ = queued_ = true;
    state_ = "queued";
    error_.clear();
    added_devices_ = added_groups_ = 0;
    email_ = std::move(email);
    password_ = std::move(password);
    return true;
}
void CloudSync::progress(const char *state) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = state;
    cloud_scale_memory(state);
}
void CloudSync::finish(const std::string &error) {
    std::lock_guard<std::mutex> lock(mutex_);
    error_ = error;
    state_ = error.empty() ? "complete" : "error";
    busy_ = false;
    erase_secret(expected_key_);
    erase_secret(password_);
    email_.clear();
    inventory_ = {};
}
std::string CloudSync::status_json() {
    std::lock_guard<std::mutex> lock(mutex_);
    JsonDocument doc;
    doc["state"] = state_;
    doc["busy"] = busy_;
    doc["error"] = error_;
    doc["added_devices"] = added_devices_;
    doc["added_groups"] = added_groups_;
#ifdef AVIONMESH_CLOUD_SCALE_TEST
    doc["experimental_synthetic"] = true;
#ifdef AVIONMESH_CLOUD_SCALE_PERSIST
    doc["dry_run"] = false;
    doc["storage_namespace"] = "avion_scale";
#else
    doc["dry_run"] = true;
#endif
#endif
    std::string json;
    serializeJson(doc, json);
    return json;
}
void CloudSync::task_entry(void *arg) {
    static_cast<CloudSync *>(arg)->worker();
    vTaskDelete(nullptr);
}
void CloudSync::worker() {
    CloudScaleSampler sampler;
    cloud_scale_memory("worker_start");
    std::string email, password, key;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        email.swap(email_);
        password.swap(password_);
        key = expected_key_;
    }
    const int64_t deadline = esp_timer_get_time() + 180000000;
    std::string error, token, login;
    CloudInventory inventory;
    JsonDocument doc;
    // SNTP is configured by the board YAML. Never authenticate with an unset
    // clock: certificate validity dates are checked as well as chain/hostname.
    progress("waiting_for_time");
    const int64_t clock_deadline = esp_timer_get_time() + 20000000;
    while (std::time(nullptr) < 1767225600 && esp_timer_get_time() < clock_deadline)
        vTaskDelay(pdMS_TO_TICKS(250));
    if (std::time(nullptr) < 1767225600) {
        erase_secret(password);
        erase_secret(key);
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "clock_not_synchronized";
        worker_done_ = true;
        return;
    }
    doc["email"] = email;
    doc["password"] = password;
    serializeJson(doc, login);
    erase_secret(password);
    email.clear();
    doc.clear();
    progress("signing_in");
    bool ok = fetch_cloud("sessions", "", &login, doc, error, deadline);
    erase_secret(login);
    if (ok) {
        token = doc["credentials"]["auth_token"] | "";
        if (token.empty() || token.size() > 4096 ||
            token.find_first_of("\r\n") != std::string::npos) {
            error = "missing_cloud_token";
            ok = false;
        }
    }
    doc.clear();
    std::vector<std::string> locations;
    if (ok) {
        progress("finding_mesh");
        ok = fetch_cloud("user/locations", token, nullptr, doc, error, deadline);
        if (ok) {
            auto list = doc["locations"].as<JsonArrayConst>();
            if (list.isNull() || list.size() > 32) {
                error = "invalid_locations";
                ok = false;
            } else
                for (JsonObjectConst item : list) {
                    std::string pid;
                    if (!cloud_resource_pid(item["pid"], pid)) {
                        error = "invalid_location_id";
                        ok = false;
                        break;
                    }
                    locations.push_back(std::move(pid));
                }
        }
    }
    std::string matched;
    unsigned matches = 0;
    for (const auto &pid : locations) {
        if (!ok)
            break;
        ok = fetch_cloud("locations/" + pid, token, nullptr, doc, error, deadline);
        if (ok && doc["location"]["passphrase"].as<std::string>() == key) {
            matched = pid;
            ++matches;
        }
    }
    if (ok && matches != 1) {
        error = "account_mesh_mismatch";
        ok = false;
    }
    erase_secret(key);
    doc.clear();
    if (ok) {
        progress("reading_devices");
        ok = fetch_cloud("locations/" + matched + "/abstract_devices", token, nullptr, doc, error,
                         deadline) &&
             cloud_parse_devices(doc.as<JsonVariantConst>(), inventory, error);
    }
    cloud_scale_memory("devices_accumulated");
    std::vector<std::string> groups;
    if (ok) {
        progress("reading_groups");
        ok = fetch_cloud("locations/" + matched + "/groups", token, nullptr, doc, error, deadline);
        if (ok) {
            auto list = doc["groups"].as<JsonArrayConst>();
            if (list.isNull() || list.size() > 128) {
                error = "invalid_groups";
                ok = false;
            } else
                for (JsonObjectConst item : list) {
                    std::string pid;
                    if (!cloud_resource_pid(item["pid"], pid)) {
                        error = "invalid_group_id";
                        ok = false;
                        break;
                    }
                    groups.push_back(std::move(pid));
                }
        }
    }
    for (const auto &pid : groups) {
        if (!ok)
            break;
        ok = fetch_cloud("groups/" + pid, token, nullptr, doc, error, deadline) &&
             cloud_parse_group(doc.as<JsonVariantConst>(), inventory, error);
        cloud_scale_memory("group_accumulated");
    }
    cloud_scale_memory("inventory_accumulated");
    erase_secret(token);
    doc.clear();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = ok ? "" : (error.empty() ? "cloud_sync_failed" : error);
        inventory_ = std::move(inventory);
        worker_done_ = true;
    }
}
bool CloudSync::loop(DeviceDB &db) {
    bool launch = false, done = false;
    std::string error;
    CloudInventory inventory;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queued_) {
            queued_ = false;
#ifdef AVIONMESH_CLOUD_SCALE_TEST
            expected_key_ = "AQEBAQEBAQEBAQEBAQEBAQ==";
#else
            expected_key_ = db.passphrase();
#endif
            launch = true;
        }
        if (worker_done_) {
            worker_done_ = false;
            done = true;
            error = error_;
            inventory = std::move(inventory_);
#ifndef AVIONMESH_CLOUD_SCALE_TEST
            if (error.empty() && expected_key_ != db.passphrase())
                error = "mesh_changed_during_sync";
#endif
        }
    }
    if (launch) {
#ifdef AVIONMESH_CLOUD_SCALE_TEST
        if (false)  // Test key is fixed; the home key is never used or transmitted.
#else
        if (db.passphrase().empty())
#endif
            finish("mesh_key_required");
        else if (xTaskCreate(task_entry, "avion_cloud", 12288, this, 1, nullptr) != pdPASS)
            finish("sync_task_allocation_failed");
    }
    if (!done)
        return false;
    cloud_scale_memory("merge_validate_before");
#ifdef AVIONMESH_CLOUD_SCALE_TEST
#ifdef AVIONMESH_CLOUD_SCALE_PERSIST
    if (!scale_loaded_) {
        scale_db_.load();
        scale_loaded_ = true;
    }
    const auto &current_devices = scale_db_.devices();
    const auto &current_groups = scale_db_.groups();
#else
    const auto &current_devices = scale_devices_;
    const auto &current_groups = scale_groups_;
#endif
#else
    const auto &current_devices = db.devices();
    const auto &current_groups = db.groups();
#endif
    if (error.empty())
        cloud_validate_merge(inventory, current_devices, current_groups, error);
    cloud_scale_memory("merge_validate_after");
    if (!error.empty()) {
        finish(error);
        return false;
    }
    progress("saving");
    unsigned added_devices = 0, added_groups = 0;
    cloud_scale_memory("merge_copy_before");
    auto devices = current_devices;
    auto groups = current_groups;
    cloud_scale_memory("merge_copy_after");
    for (const auto &d : inventory.devices) {
        if (std::none_of(current_devices.begin(), current_devices.end(),
                         [&](const DeviceEntry &old) { return old.avion_id == d.id; })) {
            devices.push_back({d.id,
                               d.product,
                               d.name,
                               {},
                               controller_ha_default(d.product),
                               keypad_setup_default(d.product)});
            ++added_devices;
        }
    }
    for (const auto &g : inventory.groups) {
        auto found = std::find_if(groups.begin(), groups.end(),
                                  [&](const GroupEntry &old) { return old.group_id == g.id; });
        if (found == groups.end()) {
            groups.push_back({g.id, g.name, {}, false});
            found = groups.end() - 1;
            ++added_groups;
        }
        if (found->ha_event_target)
            continue;
        for (auto member : g.members) {
            if (std::find(found->member_ids.begin(), found->member_ids.end(), member) ==
                found->member_ids.end())
                found->member_ids.push_back(member);
            for (auto &d : devices)
                if (d.avion_id == member &&
                    std::find(d.groups.begin(), d.groups.end(), g.id) == d.groups.end())
                    d.groups.push_back(g.id);
        }
    }
    cloud_scale_memory("merge_complete");
#ifdef AVIONMESH_CLOUD_SCALE_TEST
    // This provider is deliberately incapable of mutating the passed home DB.
#ifdef AVIONMESH_CLOUD_SCALE_PERSIST
    cloud_scale_memory("save_test_namespace_before");
    if (!scale_db_.replace_inventory_metadata(std::move(devices), std::move(groups))) {
        cloud_scale_memory("save_test_namespace_failed");
        finish("inventory_save_failed");
        return false;
    }
    cloud_scale_memory("save_test_namespace_after");
#else
    scale_devices_ = std::move(devices);
    scale_groups_ = std::move(groups);
    cloud_scale_memory("save_skipped_dry_run");
#endif
#else
    cloud_scale_memory("save_before");
    if (!db.replace_inventory_metadata(std::move(devices), std::move(groups))) {
        finish("inventory_save_failed");
        return false;
    }
    cloud_scale_memory("save_after");
#endif
    {
        std::lock_guard<std::mutex> lock(mutex_);
        added_devices_ = added_devices;
        added_groups_ = added_groups;
    }
    finish("");
#ifdef AVIONMESH_CLOUD_SCALE_TEST
    // Suppress hub cache invalidation, MQTT discovery and other real-inventory work.
    return false;
#else
    return true;
#endif
}
}  // namespace avionmesh
#endif
