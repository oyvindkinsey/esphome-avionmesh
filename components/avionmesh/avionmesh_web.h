#pragma once

#include "esphome/components/web_server_base/web_server_base.h"

#include "sse_sessions.h"

#include <atomic>
#include <string>
#include <vector>

namespace avionmesh {

class AvionMeshHub;

class AvionMeshWebHandler : public AsyncWebHandler {
 public:
    AvionMeshWebHandler(AvionMeshHub *hub) : hub_(hub) {}

    bool canHandle(AsyncWebServerRequest *request) const override;
    void handleRequest(AsyncWebServerRequest *request) override;
    // ESPHome >= 2026.x delivers JSON POST bodies here before handleRequest()
    void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,
                    size_t total) override;

    void send_event(const char *event, const std::string &data);
    void sse_loop();
    void reset_sync();

 protected:
    AvionMeshHub *hub_;

    SseSessions sse_sessions_;
    static void sse_drain_work(void *arg);
    static void sse_free_context(void *ptr);
    static int nonblocking_send(httpd_handle_t hd, int fd, const char *buf, size_t len, int flags);
    uint32_t last_state_read_ms_{0};

    std::string read_body(AsyncWebServerRequest *request);
    std::string body_buf_;
    bool body_received_{false};
    bool body_too_big_{false};

    void send_initial_sync(SseSession *session);

    void handle_index(AsyncWebServerRequest *request);
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
    void handle_controller_bindings_read(AsyncWebServerRequest *request);
    void handle_controller_setup(AsyncWebServerRequest *request);
    void handle_set_mqtt_exposed(AsyncWebServerRequest *request);
    void handle_local_dimmer_mode(AsyncWebServerRequest *request);
    void handle_save(AsyncWebServerRequest *request);
    void handle_export(AsyncWebServerRequest *request);
    void handle_rename_device(AsyncWebServerRequest *request);
    void handle_rename_group(AsyncWebServerRequest *request);
    void handle_set_passphrase(AsyncWebServerRequest *request);
    void handle_generate_passphrase(AsyncWebServerRequest *request);
    void handle_factory_reset(AsyncWebServerRequest *request);

    void send_json(AsyncWebServerRequest *request, int code, const std::string &json);
    void send_error(AsyncWebServerRequest *request, int code, const char *message);
};

}  // namespace avionmesh
