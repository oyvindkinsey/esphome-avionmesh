#pragma once

#include "esphome/components/web_server_base/web_server_base.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace avionmesh {

class AvionMeshHub;

struct SseSession {
    httpd_handle_t hd;
    std::atomic<int> fd{0};
    bool sync_pending{true};
    static void destroy(void *ptr);
};

class AvionMeshWebHandler : public AsyncWebHandler {
    static constexpr size_t MAX_SSE_SESSIONS = 2;

 public:
    AvionMeshWebHandler(AvionMeshHub *hub) : hub_(hub) {}

    bool canHandle(AsyncWebServerRequest *request) const override;
    void handleRequest(AsyncWebServerRequest *request) override;

    void send_event(const char *event, const std::string &data);
    void sse_loop();
    void reset_sync();

 protected:
    AvionMeshHub *hub_;

    std::mutex sse_mutex_;
    std::vector<SseSession *> sse_sessions_;
    uint32_t last_state_read_ms_{0};

    std::string read_body(AsyncWebServerRequest *request);

    void send_event_to(SseSession *session, const char *event, const std::string &data);
    void send_initial_sync(SseSession *session);

    void handle_index(AsyncWebServerRequest *request);
    void handle_style(AsyncWebServerRequest *request);
    void handle_script(AsyncWebServerRequest *request);
    void handle_events(AsyncWebServerRequest *request);
    void handle_discover_mesh_post(AsyncWebServerRequest *request);
    void handle_scan_unassociated_post(AsyncWebServerRequest *request);
    void handle_claim_device(AsyncWebServerRequest *request);
    void handle_add_discovered(AsyncWebServerRequest *request);
    void handle_unclaim_device(AsyncWebServerRequest *request);
    void handle_examine_device_post(AsyncWebServerRequest *request);
    void handle_control(AsyncWebServerRequest *request);
    void handle_create_group(AsyncWebServerRequest *request);
    void handle_delete_group(AsyncWebServerRequest *request);
    void handle_add_to_group(AsyncWebServerRequest *request);
    void handle_remove_from_group(AsyncWebServerRequest *request);
    void handle_import(AsyncWebServerRequest *request);
    void handle_set_mqtt_exposed(AsyncWebServerRequest *request);
    void handle_save(AsyncWebServerRequest *request);
    void handle_set_passphrase(AsyncWebServerRequest *request);
    void handle_generate_passphrase(AsyncWebServerRequest *request);
    void handle_factory_reset(AsyncWebServerRequest *request);

    void send_json(AsyncWebServerRequest *request, int code, const std::string &json);
    void send_error(AsyncWebServerRequest *request, int code, const char *message);
};

}  // namespace avionmesh
