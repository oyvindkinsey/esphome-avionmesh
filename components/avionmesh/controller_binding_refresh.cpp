#include "avionmesh_hub.h"
#ifdef USE_ESP32
#include "avionmesh_web.h"
#endif
#include "esphome/components/json/json_util.h"
#include "esphome/core/helpers.h"
#include <cstring>

namespace avionmesh {
std::string AvionMeshHub::controller_bindings_json(uint16_t target) {
    const auto *dev = db_.find_device(target);
    const auto *snapshot = controller_bindings_.find(target);
    return esphome::json::build_json([&](JsonObject root) {
        root["controller_id"] = target;
        const uint8_t product = dev ? dev->product_type : 0;
        root["assignment_read_supported"] = keypad_assignment_model_supported(product);
        root["assignment_write_supported"] = keypad_assignment_write_supported(product);
        root["wake_required"] = product == 126;
        root["programming_status"] = keypad_programming_status(product);
        root["read_only"] = !(keypad_setup_active_ && keypad_setup_target_ == target);
        root["setup_status"] = keypad_setup_status(target);
        root["setup_busy"] = keypad_setup_active_ && keypad_setup_target_ == target;
        root["setup_pending"] = dev && dev->ha_setup_pending;
        if (keypad_setup_active_ && keypad_setup_target_ == target)
            root["setup_slot"] = keypad_setup_slot_;
        root["busy"] = binding_refresh_active_ && binding_refresh_target_ == target;
        auto state = binding_refresh_status_.find(target);
        root["status"] = binding_refresh_active_ && binding_refresh_target_ == target
                             ? (binding_refresh_waiting_wake_ ? "waiting_for_wake" : "reading")
                             : (state != binding_refresh_status_.end() ? state->second.c_str()
                                : snapshot && snapshot->valid          ? "saved"
                                                                       : "needs_refresh");
        if (binding_refresh_active_ && binding_refresh_target_ == target) {
            root["round"] = binding_refresh_round_ + 1;
            root["query_slot"] = binding_refresh_slot_;
            if (binding_refresh_product_ == 126) {
                root["wake_reason"] = binding_refresh_wake_reason_;
                root["wake_retries"] = binding_refresh_wake_retries_;
                root["read_session_timeout_ms"] = 180000;
            }
        }
        if (!dev || (dev->product_type != 127 && dev->product_type != 126))
            return;
        auto slots = root["slots"].to<JsonArray>();
        for (uint8_t slot = 1; slot <= 4; ++slot) {
            auto item = slots.add<JsonObject>();
            item["slot"] = slot;
            item["status"] = binding_slot_status(snapshot, dev->product_type, slot);
            if (snapshot && snapshot->valid && snapshot->product == dev->product_type) {
                const auto &r = snapshot->records[slot - 1];
                item["target_id"] = binding_target(r);
                item["brightness"] = r[7];
            }
        }
    });
}
void AvionMeshHub::publish_controller_actions(uint16_t target) {
    const auto *dev = db_.find_device(target);
    if (dev && dev->mqtt_exposed && is_controller(dev->product_type)) {
        if (dev->product_type == 91 && dev->ha_local_level) {
            const auto found = controller_local_levels_.find(target);
            const bool ready = found != controller_local_levels_.end() && found->second.ready();
            do_mqtt_publish(discovery_.controller_input_availability_topic(target),
                            ready ? "online" : "offline", true);
        }
        discovery_.publish_controller_actions(target, dev->product_type, dev->name,
                                              controller_bindings_.find(target),
                                              dev->ha_local_level);
    }
}
void AvionMeshHub::start_controller_binding_refresh(uint16_t target) {
    const auto *dev = db_.find_device(target);
    if (!dev || (dev->product_type != 127 && dev->product_type != 126)) {
        binding_refresh_status_[target] = "model_unverified";
        do_sse_emit("controller_buttons", controller_bindings_json(target));
        return;
    }
    if (!mesh_initialized_ || (keypad_setup_active_ && !keypad_setup_internal_) ||
        binding_refresh_active_ || controller_read_active_ || controller_program_active_ ||
        associating_ || discovering_mesh_ || examining_ || scanning_unassociated_) {
        binding_refresh_status_[target] = "busy_or_disconnected";
        do_sse_emit("controller_buttons", controller_bindings_json(target));
        return;
    }
    binding_refresh_target_ = target;
    binding_refresh_product_ = dev->product_type;
    binding_refresh_mesh_tag_ = controller_mesh_tag(db_.passphrase());
    binding_refresh_round_ = binding_refresh_slot_ = binding_refresh_attempt_ = 0;
    binding_refresh_records_ = {};
    binding_refresh_counts_ = {};
    binding_refresh_active_ = true;
    binding_refresh_waiting_ = false;
    binding_refresh_waiting_wake_ = dev->product_type == 126;
    binding_refresh_wake_retries_ = 0;
    binding_refresh_wake_reason_ = "initial_round";
    binding_refresh_started_ms_ = binding_refresh_wait_since_ms_ = esphome::millis();
    binding_refresh_status_[target] =
        binding_refresh_waiting_wake_ ? "waiting_for_wake" : "reading";
    controller_bindings_.invalidate(target);
    publish_controller_actions(target);
    do_sse_emit("controller_buttons", controller_bindings_json(target));
}
void AvionMeshHub::handle_battery_keypad_wake(uint16_t source, uint8_t opcode,
                                              const uint8_t *payload, size_t length,
                                              uint32_t received) {
    // Called only after the receive path authenticates source. A queued packet
    // from before the user's read request must not open a new receive window.
    const auto *device = db_.find_device(source);
    if (!device || device->product_type != 126 ||
        !battery_keypad_wake_packet(opcode, payload, length, received, esphome::millis()))
        return;
    if (keypad_setup_active_ && keypad_setup_target_ == source &&
        keypad_setup_phase_ == KeypadSetupPhase::WaitWake &&
        static_cast<int32_t>(received - keypad_setup_wait_since_ms_) >= 0 &&
        static_cast<uint32_t>(esphome::millis() - keypad_setup_wait_since_ms_) < 180000) {
        keypad_setup_phase_ = KeypadSetupPhase::Baseline;
        keypad_setup_status_[source] = "reading_assignments";
        keypad_setup_emit();
    }
    if (binding_refresh_active_ && binding_refresh_target_ == source &&
        binding_refresh_waiting_wake_ &&
        static_cast<int32_t>(received - binding_refresh_wait_since_ms_) >= 0 &&
        static_cast<uint32_t>(esphome::millis() - binding_refresh_started_ms_) < 180000) {
        binding_refresh_waiting_wake_ = false;
        binding_refresh_wait_since_ms_ = esphome::millis();
        binding_refresh_status_[source] = "reading";
        do_sse_emit("controller_buttons", controller_bindings_json(source));
    }
}
uint32_t AvionMeshHub::controller_read_timeout_ms() const {
    const auto *device = db_.find_device(controller_read_target_);
    // Mapping retries stay inside the short battery receive window. Diagnostic
    // probes and programming retain their separate policies.
    if (controller_read_probe_)
        return 2500;
    return binding_refresh_active_ && device && device->product_type == 126 ? 750 : 8000;
}
bool AvionMeshHub::controller_read_reply_is_current(uint16_t source, uint8_t opcode,
                                                    const uint8_t *payload, size_t length,
                                                    uint32_t received) const {
    return controller_read_active_ && payload &&
           (!binding_refresh_active_ || binding_refresh_product_ != 126 ||
            static_cast<uint32_t>(esphome::millis() - binding_refresh_started_ms_) < 180000) &&
           static_cast<int32_t>(received - controller_read_since_ms_) >= 0 &&
           static_cast<uint32_t>(esphome::millis() - controller_read_since_ms_) <
               controller_read_timeout_ms() &&
           (controller_read_probe_
                ? controller_probe_reply_matches(controller_read_target_, controller_probe_noun_,
                                                 controller_probe_count_, source, opcode, payload,
                                                 length)
                : controller_table_reply_matches(controller_read_target_, controller_read_slot_,
                                                 source, opcode, payload, length));
}
void AvionMeshHub::wait_for_battery_binding_wake(const char *reason) {
    binding_refresh_waiting_wake_ = true;
    binding_refresh_wait_since_ms_ = esphome::millis();
    binding_refresh_wake_reason_ = reason;
    binding_refresh_status_[binding_refresh_target_] = "waiting_for_wake";
    do_sse_emit("controller_buttons", controller_bindings_json(binding_refresh_target_));
}
void AvionMeshHub::finish_controller_binding_refresh(const char *status) {
    binding_refresh_active_ = binding_refresh_waiting_ = false;
    binding_refresh_waiting_wake_ = false;
    if (controller_read_active_ && controller_read_id_ == binding_refresh_request_ &&
        controller_read_target_ == binding_refresh_target_)
        report_controller_read("cancelled");
    binding_refresh_status_[binding_refresh_target_] = status;
    publish_controller_actions(binding_refresh_target_);
    do_sse_emit("controller_buttons", controller_bindings_json(binding_refresh_target_));
    keypad_setup_mapping_finished(binding_refresh_target_, status);
}
void AvionMeshHub::process_controller_binding_refresh() {
    if (!binding_refresh_active_)
        return;
    if (!mesh_initialized_ || controller_mesh_tag(db_.passphrase()) != binding_refresh_mesh_tag_) {
        finish_controller_binding_refresh("disconnected_or_mesh_changed");
        return;
    }
    const auto *device = db_.find_device(binding_refresh_target_);
    if (!device || device->product_type != binding_refresh_product_) {
        finish_controller_binding_refresh("controller_removed_or_model_changed");
        return;
    }
    if (device->product_type == 126 &&
        static_cast<uint32_t>(esphome::millis() - binding_refresh_started_ms_) >= 180000) {
        finish_controller_binding_refresh(binding_refresh_waiting_wake_
                                              ? "wake_timeout_try_read_again"
                                              : "read_session_timeout_try_again");
        return;
    }
    if (binding_refresh_waiting_wake_)
        return;
    // Let the keypad finish its wake announcement before the first request.
    if (device->product_type == 126 &&
        static_cast<uint32_t>(esphome::millis() - binding_refresh_wait_since_ms_) < 150)
        return;
    if (binding_refresh_waiting_ || controller_read_active_)
        return;
    if (controller_program_active_ || associating_ || discovering_mesh_ || examining_ ||
        scanning_unassociated_) {
        finish_controller_binding_refresh("interrupted_by_other_operation");
        return;
    }
    Command cmd;
    if (!build_controller_table_read(binding_refresh_target_, binding_refresh_slot_, cmd.payload,
                                     cmd.payload_len)) {
        finish_controller_binding_refresh("invalid_read_request");
        return;
    }
    controller_read_target_ = binding_refresh_target_;
    controller_read_slot_ = binding_refresh_slot_;
    controller_read_id_ = ++binding_refresh_request_;
    controller_read_since_ms_ = esphome::millis();
    controller_read_active_ = binding_refresh_waiting_ = true;
    cmd.dest_id = binding_refresh_target_;
    report_controller_read("started");
    do_mesh_send(cmd);  // COUNT/READ only. Never programs an assignment.
}
void AvionMeshHub::binding_refresh_reply(const char *status, const uint8_t *payload,
                                         size_t length) {
    if (!binding_refresh_active_ || !binding_refresh_waiting_ ||
        controller_read_id_ != binding_refresh_request_ ||
        controller_read_target_ != binding_refresh_target_ || !std::strcmp(status, "started"))
        return;
    binding_refresh_waiting_ = false;
    if (std::strcmp(status, "reply")) {
        if (!std::strcmp(status, "timeout") && mesh_initialized_) {
            if (binding_refresh_product_ == 126) {
                if (++binding_refresh_wake_retries_ <= 2) {
                    // Never assemble a round from different awake windows.
                    binding_refresh_slot_ = binding_refresh_attempt_ = 0;
                    binding_refresh_records_[binding_refresh_round_] = {};
                    binding_refresh_counts_[binding_refresh_round_] = {};
                    wait_for_battery_binding_wake("read_timeout_restart_round");
                    return;
                }
            } else if (++binding_refresh_attempt_ <= 2)
                return;
        }
        finish_controller_binding_refresh("read_failed");
        return;
    }
    binding_refresh_attempt_ = 0;
    if (!binding_refresh_slot_) {
        if (length != 5 || !payload || payload[0] != 4 || payload[1] != 0x30 || payload[2] ||
            payload[3] > 4 || payload[4] != 4) {
            finish_controller_binding_refresh("unsupported_count_record");
            return;
        }
        std::copy_n(payload, 5, binding_refresh_counts_[binding_refresh_round_].begin());
    } else {
        if (!payload ||
            !controller_table_reply_matches(binding_refresh_target_, binding_refresh_slot_,
                                            binding_refresh_target_, 0x73, payload, length)) {
            finish_controller_binding_refresh("invalid_slot_record");
            return;
        }
        std::copy_n(
            payload, 8,
            binding_refresh_records_[binding_refresh_round_][binding_refresh_slot_ - 1].begin());
    }
    if (++binding_refresh_slot_ > 4) {
        if (++binding_refresh_round_ == 2) {
            if (binding_refresh_counts_[0] != binding_refresh_counts_[1] ||
                binding_refresh_records_[0] != binding_refresh_records_[1]) {
                finish_controller_binding_refresh("assignments_changed_during_read");
                return;
            }
            if (!valid_binding_records(binding_refresh_records_[0]) ||
                binding_refresh_counts_[0][3] !=
                    binding_assigned_count(binding_refresh_records_[0])) {
                finish_controller_binding_refresh("count_does_not_match_assignments");
                return;
            }
            const auto *device = db_.find_device(binding_refresh_target_);
            const bool saved =
                device && controller_bindings_.set(binding_refresh_target_, device->product_type,
                                                   binding_refresh_records_[0]);
            finish_controller_binding_refresh(saved ? "saved" : "save_failed");
            return;
        }
        binding_refresh_slot_ = 0;
        if (binding_refresh_product_ == 126) {
            // The observed awake interval fits one round, not two. A new wake
            // is required for the independent complete verification round.
            wait_for_battery_binding_wake("verification_round");
            return;
        }
    }
    do_sse_emit("controller_buttons", controller_bindings_json(binding_refresh_target_));
}
}  // namespace avionmesh
