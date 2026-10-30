#include "cloud_sync.h"

#ifdef USE_ESP32
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ctime>
#include "esphome/core/log.h"

namespace avionmesh {
namespace {
void erase_secret(std::string &value) {
    volatile char *p = value.empty() ? nullptr : &value[0];
    for (size_t i = 0; i < value.size(); ++i)
        p[i] = 0;
    value.clear();
}
struct Response {
    std::string body;
    bool too_big{false};
    int64_t deadline;
};
esp_err_t receive_http(esp_http_client_event_t *event) {
    auto &r = *static_cast<Response *>(event->user_data);
    if (esp_timer_get_time() > r.deadline)
        return ESP_FAIL;
    if (event->event_id == HTTP_EVENT_ON_DATA) {
        if (event->data_len < 0 || r.body.size() + event->data_len > 262144) {
            r.too_big = true;
            return ESP_FAIL;
        }
        r.body.append(static_cast<const char *>(event->data), event->data_len);
    }
    return ESP_OK;
}
bool fetch_cloud(const std::string &path, const std::string &token, const std::string *login,
                 JsonDocument &doc, std::string &error, int64_t deadline) {
    if (esp_timer_get_time() > deadline) {
        error = "sync_timeout";
        return false;
    }
    // Only fixed-host HTTPS; never follow a redirect with credentials or tokens.
    const std::string url = "https://api.avi-on.com/" + path;
    Response response{{}, false, deadline};
    esp_http_client_config_t config{};
    config.url = url.c_str();
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.disable_auto_redirect = true;
    config.max_authorization_retries = -1;  // Token auth only; no HTTP auth negotiation.
    config.timeout_ms = 15000;
    config.event_handler = receive_http;
    config.user_data = &response;
    auto client = esp_http_client_init(&config);
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
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    erase_secret(authorization);
    // ESP-IDF can return ESP_FAIL for a perfectly valid HTTP 401 when its
    // built-in HTTP auth negotiation is disabled. Classify the status first.
    if (status == 401 || status == 403 || (login && status == 400)) {
        error = "cloud_login_rejected";
        erase_secret(response.body);
        return false;
    }
    if (result != ESP_OK || response.too_big) {
        ESP_LOGW("avionmesh.cloud", "HTTPS request failed (result=%s, status=%d)",
                 esp_err_to_name(result), status);
        error = response.too_big ? "cloud_response_too_large" : "cloud_connection_failed";
        erase_secret(response.body);
        return false;
    }
    if (status != 200 && status != 201) {
        error = "cloud_http_" + std::to_string(status);
        erase_secret(response.body);
        return false;
    }
    doc.clear();
    const auto parsed =
        deserializeJson(doc, response.body, DeserializationOption::NestingLimit(16));
    erase_secret(response.body);
    if (parsed || !doc.is<JsonObject>()) {
        error = "invalid_cloud_response";
        return false;
    }
    return true;
}
}  // namespace

bool CloudSync::queue(std::string email, std::string password) {
    std::lock_guard<std::mutex> lock(mutex_);
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
    std::string json;
    serializeJson(doc, json);
    return json;
}
void CloudSync::task_entry(void *arg) {
    static_cast<CloudSync *>(arg)->worker();
    vTaskDelete(nullptr);
}
void CloudSync::worker() {
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
    }
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
            expected_key_ = db.passphrase();
            launch = true;
        }
        if (worker_done_) {
            worker_done_ = false;
            done = true;
            error = error_;
            inventory = std::move(inventory_);
            if (error.empty() && expected_key_ != db.passphrase())
                error = "mesh_changed_during_sync";
        }
    }
    if (launch) {
        if (db.passphrase().empty())
            finish("mesh_key_required");
        else if (xTaskCreate(task_entry, "avion_cloud", 12288, this, 1, nullptr) != pdPASS)
            finish("sync_task_allocation_failed");
    }
    if (!done)
        return false;
    if (error.empty())
        cloud_validate_merge(inventory, db.devices(), db.groups(), error);
    if (!error.empty()) {
        finish(error);
        return false;
    }
    progress("saving");
    unsigned added_devices = 0, added_groups = 0;
    auto devices = db.devices();
    auto groups = db.groups();
    for (const auto &d : inventory.devices) {
        if (!db.find_device(d.id)) {
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
    if (!db.replace_inventory_metadata(std::move(devices), std::move(groups))) {
        finish("inventory_save_failed");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        added_devices_ = added_devices;
        added_groups_ = added_groups;
    }
    finish("");
    return true;
}
}  // namespace avionmesh
#endif
