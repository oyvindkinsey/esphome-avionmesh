#pragma once
#include "sse_sessions.h"
#include "json_string.h"
#include "controller_events.h"
#include "controller_profile.h"
#include <avionmesh/types.h>
#include <algorithm>

namespace avionmesh {
// Shared production serializer/cursor, exercised with actual inventory fixtures.
// Caller holds SseSessions' lock; one frame is enqueued per main-loop turn.
template <typename DB, typename States, typename ControllerJson, typename InputStatus>
void send_sse_initial_sync(SseSession *session, const DB &db, const States &states,
                           const std::string &meta, bool mesh_mqtt_exposed,
                           ControllerJson controller_json, InputStatus input_status) {
    if (!session->ready_for_sync())
        return;

    // One accepted frame per main-loop turn; cursors never skip a rejected frame.
    if (session->sync_phase == SseSession::SyncPhase::Meta) {
        if (session->send("meta", meta)) {
            session->sync_phase = SseSession::SyncPhase::Devices;
            session->sync_index = 0;
        }
        return;
    }

    // Devices in batches of five.
    if (session->sync_phase == SseSession::SyncPhase::Devices) {
        auto &devs = db.devices();
        static constexpr size_t BATCH = 5;
        const size_t i = session->sync_index;
        if (i < devs.size()) {
            std::string json = "{\"devices\":[";
            size_t end = std::min(i + BATCH, devs.size());
            for (size_t j = i; j < end; j++) {
                auto &dev = devs[j];
                if (j > i)
                    json += ",";
                json += "{\"avion_id\":";
                json += std::to_string(dev.avion_id);
                json += ",\"name\":";
                append_json_string(json, dev.name);
                json += ",\"product_type\":";
                json += std::to_string(dev.product_type);
                json += ",\"product_name\":\"";
                json += is_controller(dev.product_type) ? controller_name(dev.product_type)
                                                        : product_name(dev.product_type);
                json += "\",\"is_controller\":";
                json += is_controller(dev.product_type) ? "true" : "false";
                json += ",\"groups\":[";
                for (size_t g = 0; g < dev.groups.size(); g++) {
                    if (g > 0)
                        json += ",";
                    json += std::to_string(dev.groups[g]);
                }
                json += "],\"mqtt_exposed\":";
                json += dev.mqtt_exposed ? "true" : "false";
                json += ",\"ha_setup_pending\":";
                json += dev.ha_setup_pending ? "true" : "false";
                json += ",\"ha_local_level\":";
                json += dev.ha_local_level ? "true" : "false";
                json += ",\"ha_input_status\":\"";
                json += input_status(dev.avion_id);
                json += "\"";
                if (dimmer_profile(dev.product_type).supported) {
                    json += ",\"ha_input_mode\":\"";
                    json += dimmer_input_mode(dev.product_type, dev.ha_local_level);
                    json += "\",\"ha_native_assignment_status\":\"unknown\"";
                    json += ",\"ha_native_assignment_programming\":false";
                }
                json += ",\"has_dimming\":";
                json += has_dimming(dev.product_type) ? "true" : "false";
                json += ",\"has_color_temp\":";
                json += has_color_temp(dev.product_type) ? "true" : "false";

                auto sit = states.find(dev.avion_id);
                if (!is_controller(dev.product_type) && sit != states.end() &&
                    sit->second.brightness_known) {
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
            if (session->send("devices", json))
                session->sync_index = end;
            return;
        }
        session->sync_phase = SseSession::SyncPhase::Controllers;
        session->sync_index = 0;
    }

    // Saved per-controller button maps are separate from light/group metadata.
    if (session->sync_phase == SseSession::SyncPhase::Controllers) {
        const auto &devices = db.devices();
        while (session->sync_index < devices.size()) {
            const auto &dev = devices[session->sync_index];
            if (is_controller(dev.product_type)) {
                if (session->send("controller_buttons", controller_json(dev.avion_id)))
                    ++session->sync_index;
                return;
            }
            ++session->sync_index;
        }
        session->sync_phase = SseSession::SyncPhase::Groups;
        session->sync_index = 0;
    }

    // One group per frame bounds dense membership snapshots without changing
    // the existing incremental groups event protocol.
    if (session->sync_phase == SseSession::SyncPhase::Groups) {
        auto &grps = db.groups();
        static constexpr size_t BATCH = 1;
        const size_t i = session->sync_index;
        if (i < grps.size()) {
            std::string json = "{\"groups\":[";
            size_t end = std::min(i + BATCH, grps.size());
            for (size_t j = i; j < end; j++) {
                auto &grp = grps[j];
                if (j > i)
                    json += ",";
                json += "{\"group_id\":";
                json += std::to_string(grp.group_id);
                json += ",\"name\":";
                append_json_string(json, grp.name);
                json += ",\"members\":[";
                for (size_t m = 0; m < grp.member_ids.size(); m++) {
                    if (m > 0)
                        json += ",";
                    json += std::to_string(grp.member_ids[m]);
                }
                json += "],\"mqtt_exposed\":";
                json += grp.mqtt_exposed ? "true" : "false";
                json += ",\"ha_event_target\":";
                json += grp.ha_event_target ? "true" : "false";
                json += "}";
            }
            json += "]}";
            if (session->send("groups", json))
                session->sync_index = end;
            return;
        }
        session->sync_phase = SseSession::SyncPhase::Mesh;
    }

    // Mesh broadcast entity status
    if (session->sync_phase == SseSession::SyncPhase::Mesh) {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"mesh_mqtt_exposed\":%s}",
                 mesh_mqtt_exposed ? "true" : "false");
        if (session->send("mesh_status", buf))
            session->sync_phase = SseSession::SyncPhase::Complete;
        return;
    }

    if (session->send("sync_complete", "{}"))
        session->sync_pending = false;
}
}  // namespace avionmesh
