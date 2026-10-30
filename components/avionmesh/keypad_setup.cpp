#include "avionmesh_hub.h"
#ifdef USE_ESP32
#include "avionmesh_web.h"
#endif
#include "esphome/components/json/json_util.h"
#include <cstring>

namespace avionmesh {
std::string AvionMeshHub::keypad_setup_status(uint16_t target) const {
    const auto found = keypad_setup_status_.find(target);
    if (found != keypad_setup_status_.end())
        return found->second;
    if (keypad_setup_journal_.active && keypad_setup_journal_.controller == target)
        return "interrupted_needs_reconciliation";
    const auto *snapshot = controller_bindings_.find(target);
    if (snapshot && snapshot->valid) {
        bool ready = true;
        for (uint8_t slot = 1; slot <= 4; ++slot)
            ready = ready &&
                    std::string(binding_slot_status(snapshot, snapshot->product, slot)) == "ready";
        if (ready)
            return "ready";
    }
    return "not_requested";
}
void AvionMeshHub::keypad_setup_emit() {
    do_sse_emit("controller_buttons", controller_bindings_json(keypad_setup_target_));
}
void AvionMeshHub::finish_keypad_setup(const std::string &status) {
    keypad_setup_status_[keypad_setup_target_] = status;
    keypad_setup_active_ = false;
    publish_controller_actions(keypad_setup_target_);
    keypad_setup_emit();
#ifdef USE_ESP32
    if (web_handler_)
        web_handler_->reset_sync();
#endif
}
void AvionMeshHub::start_keypad_setup(uint16_t target, bool manual) {
    auto *dev = db_.find_device(target);
    if (!dev || dev->product_type != 127) {
        keypad_setup_status_[target] = "model_unverified";
        keypad_setup_attempted_.insert(target);
        do_sse_emit("controller_buttons", controller_bindings_json(target));
        return;
    }
    if (!mesh_initialized_ || keypad_setup_active_ || binding_refresh_active_ ||
        controller_read_active_ || controller_program_active_ || associating_ ||
        discovering_mesh_ || examining_ || scanning_unassociated_) {
        keypad_setup_status_[target] = "busy_or_disconnected";
        do_sse_emit("controller_buttons", controller_bindings_json(target));
        return;
    }
    if (keypad_setup_journal_.active && (!manual || keypad_setup_journal_.controller != target)) {
        keypad_setup_status_[target] = "interrupted_setup_requires_manual_reconciliation";
        keypad_setup_attempted_.insert(target);
        do_sse_emit("controller_buttons", controller_bindings_json(target));
        return;
    }
    const bool old_pending = dev->ha_setup_pending, old_exposure = dev->mqtt_exposed;
    dev->ha_setup_pending = dev->mqtt_exposed = true;
    if (!db_.save()) {
        dev->ha_setup_pending = old_pending;
        dev->mqtt_exposed = old_exposure;
        keypad_setup_status_[target] = "save_failed";
        do_sse_emit("controller_buttons", controller_bindings_json(target));
        return;
    }
    keypad_setup_active_ = true;
    keypad_setup_target_ = target;
    keypad_setup_mesh_tag_ = controller_mesh_tag(db_.passphrase());
    keypad_setup_reconcile_ = keypad_setup_journal_.active;
    keypad_setup_phase_ =
        dev->product_type == 126 ? KeypadSetupPhase::WaitWake : KeypadSetupPhase::Baseline;
    keypad_setup_wait_since_ms_ = esphome::millis();
    keypad_setup_slot_ = 0;
    keypad_setup_status_[target] =
        dev->product_type == 126 ? "waiting_for_wake" : "reading_assignments";
    keypad_setup_attempted_.insert(target);
    publish_all_discovery();
    keypad_setup_emit();
}
void AvionMeshHub::keypad_setup_mapping_finished(uint16_t target, const char *status) {
    if (!keypad_setup_active_ || target != keypad_setup_target_)
        return;
    if (keypad_setup_phase_ == KeypadSetupPhase::WaitBaseline ||
        keypad_setup_phase_ == KeypadSetupPhase::WaitVerify) {
        keypad_setup_mapping_done_ = true;
        keypad_setup_mapping_ok_ = !std::strcmp(status, "saved");
    }
}
void AvionMeshHub::keypad_setup_read_reply(const char *status, const uint8_t *payload,
                                           size_t length) {
    if (!keypad_setup_active_ || keypad_setup_phase_ != KeypadSetupPhase::WaitSlotRead ||
        controller_read_id_ != keypad_setup_request_ || !std::strcmp(status, "started"))
        return;
    keypad_setup_slot_done_ = true;
    keypad_setup_slot_ok_ = !std::strcmp(status, "reply") && payload && length == 8;
    if (keypad_setup_slot_ok_)
        std::copy_n(payload, 8, keypad_setup_slot_reply_.begin());
}
void AvionMeshHub::keypad_setup_program_reply(const char *status) {
    if (!keypad_setup_active_ || keypad_setup_phase_ != KeypadSetupPhase::WaitProgram ||
        controller_program_id_ != keypad_setup_request_ || !std::strcmp(status, "started") ||
        !std::strcmp(status, "primary_reply"))
        return;
    keypad_setup_program_done_ = true;  // ACK or uncertain: outcome is established by READs.
}
void AvionMeshHub::process_keypad_setup() {
    if (controller_read_active_ && controller_read_probe_)
        return;
    if (keypad_setup_journal_.active &&
        keypad_setup_journal_.mesh_tag != controller_mesh_tag(db_.passphrase())) {
        if (keypad_setup_active_)
            finish_keypad_setup("mesh_changed_setup_stopped");
        keypad_setup_journal_.load(db_.passphrase());
        keypad_setup_attempted_.clear();
        return;
    }
    if (!keypad_setup_active_) {
        if (!mesh_initialized_ || binding_refresh_active_ || controller_read_active_ ||
            controller_program_active_ || associating_ || discovering_mesh_ || examining_ ||
            scanning_unassociated_)
            return;
        for (const auto &dev : db_.devices()) {
            if (dev.ha_setup_pending && dev.mqtt_exposed &&
                !keypad_setup_attempted_.count(dev.avion_id)) {
                const uint16_t id = dev.avion_id;
                start_keypad_setup(id, false);
                break;
            }
        }
        return;
    }
    if (!mesh_initialized_ || controller_mesh_tag(db_.passphrase()) != keypad_setup_mesh_tag_ ||
        !db_.find_device(keypad_setup_target_)) {
        finish_keypad_setup("interrupted_needs_reconciliation");
        return;
    }
    if (!db_.find_device(keypad_setup_target_)->mqtt_exposed) {
        finish_keypad_setup("stopped_ha_events_disabled");
        return;
    }
    auto dispatch = [&](const std::string &message) {
        keypad_setup_internal_ = true;
        on_mqtt_command(message);
        keypad_setup_internal_ = false;
    };
    auto start_mapping_read = [&](KeypadSetupPhase waiting) {
        keypad_setup_mapping_done_ = keypad_setup_mapping_ok_ = false;
        keypad_setup_phase_ = waiting;
        keypad_setup_internal_ = true;
        start_controller_binding_refresh(keypad_setup_target_);
        keypad_setup_internal_ = false;
        if (!binding_refresh_active_)
            finish_keypad_setup("assignment_read_not_started");
    };
    switch (keypad_setup_phase_) {
        case KeypadSetupPhase::WaitWake:
            if (static_cast<uint32_t>(esphome::millis() - keypad_setup_wait_since_ms_) >= 180000)
                finish_keypad_setup("wake_timeout_try_setup_again");
            break;
        case KeypadSetupPhase::Baseline:
            start_mapping_read(KeypadSetupPhase::WaitBaseline);
            break;
        case KeypadSetupPhase::WaitBaseline: {
            if (!keypad_setup_mapping_done_)
                break;
            const auto *snapshot = controller_bindings_.find(keypad_setup_target_);
            if (!keypad_setup_mapping_ok_ || !snapshot || !snapshot->valid ||
                binding_refresh_counts_[0][3] != keypad_assigned_count(snapshot->records)) {
                finish_keypad_setup("assignment_read_failed_no_writes");
                break;
            }
            if (keypad_setup_reconcile_) {
                if (!keypad_setup_journal_.reconciles(snapshot->records)) {
                    finish_keypad_setup("interrupted_setup_changed_requires_review");
                    break;
                }
                if (!keypad_setup_journal_.clear()) {
                    finish_keypad_setup("journal_save_failed");
                    break;
                }
            }
            keypad_setup_working_ = snapshot->records;
            keypad_setup_phase_ = KeypadSetupPhase::Plan;
            break;
        }
        case KeypadSetupPhase::Plan: {
            keypad_setup_status_[keypad_setup_target_] = "planning_targets";
            keypad_setup_emit();
            std::string error;
            if (!plan_unused_keypad_buttons(
                    keypad_setup_target_, keypad_setup_working_, db_.devices(), db_.groups(),
                    controller_bindings_.known_targets(), keypad_setup_plan_, error)) {
                finish_keypad_setup(error);
                break;
            }
            if (!keypad_setup_plan_.write_mask) {
                keypad_setup_phase_ = KeypadSetupPhase::Finish;
                break;
            }
            // Persist both original and proposed tables before any configuration write.
            if (!keypad_setup_journal_.begin(keypad_setup_plan_, db_.passphrase())) {
                finish_keypad_setup("journal_save_failed");
                break;
            }
            auto groups = db_.groups();
            groups.insert(groups.end(), keypad_setup_plan_.new_groups.begin(),
                          keypad_setup_plan_.new_groups.end());
            if (!db_.replace_inventory_metadata(db_.devices(), std::move(groups))) {
                finish_keypad_setup("target_reservation_save_failed");
                break;
            }
#ifdef USE_ESP32
            if (web_handler_)
                web_handler_->reset_sync();
#endif
            keypad_setup_slot_ = 0;
            keypad_setup_phase_ = KeypadSetupPhase::SlotRead;
            break;
        }
        case KeypadSetupPhase::SlotRead: {
            unsigned next = keypad_setup_slot_ + 1;
            while (next <= 4 && !(keypad_setup_plan_.write_mask & (1u << (next - 1))))
                ++next;
            if (next > 4) {
                keypad_setup_phase_ = KeypadSetupPhase::Finish;
                break;
            }
            keypad_setup_slot_ = next;
            keypad_setup_status_[keypad_setup_target_] = "checking_unused_button";
            keypad_setup_emit();
            keypad_setup_slot_done_ = keypad_setup_slot_ok_ = false;
            keypad_setup_phase_ = KeypadSetupPhase::WaitSlotRead;
            auto message = esphome::json::build_json([&](JsonObject root) {
                root["action"] = "controller_read";
                root["target"] = keypad_setup_target_;
                root["slot"] = keypad_setup_slot_;
                root["request_id"] = ++keypad_setup_request_;
            });
            dispatch(message);
            if (!controller_read_active_)
                finish_keypad_setup("fresh_slot_read_not_started");
            break;
        }
        case KeypadSetupPhase::WaitSlotRead:
            if (!keypad_setup_slot_done_)
                break;
            if (!keypad_setup_slot_ok_ ||
                keypad_setup_slot_reply_ != keypad_setup_working_[keypad_setup_slot_ - 1]) {
                finish_keypad_setup("slot_changed_before_write");
                break;
            }
            keypad_setup_phase_ = KeypadSetupPhase::Program;
            break;
        case KeypadSetupPhase::Program: {
            const auto group_id = keypad_setup_plan_.targets[keypad_setup_slot_ - 1];
            const auto *group = db_.find_group(group_id);
            if (!group || group->mqtt_exposed || !group->member_ids.empty() ||
                group->name !=
                    controller_event_target_name(keypad_setup_target_, keypad_setup_slot_)) {
                finish_keypad_setup("unsafe_reserved_target");
                break;
            }
            if (!keypad_setup_journal_.mark_attempt(keypad_setup_slot_)) {
                finish_keypad_setup("journal_save_failed");
                break;
            }
            keypad_setup_program_done_ = false;
            keypad_setup_phase_ = KeypadSetupPhase::WaitProgram;
            keypad_setup_status_[keypad_setup_target_] = "programming_unused_button";
            keypad_setup_emit();
            auto message = esphome::json::build_json([&](JsonObject root) {
                root["action"] = "controller_program";
                root["target"] = keypad_setup_target_;
                root["slot"] = keypad_setup_slot_;
                root["group_id"] = group_id;
                root["expected_record"] = controller_unused_record(keypad_setup_slot_);
                root["request_id"] = ++keypad_setup_request_;
            });
            dispatch(message);
            if (!controller_program_active_)
                finish_keypad_setup("program_not_started_reconcile_required");
            break;
        }
        case KeypadSetupPhase::WaitProgram:
            if (keypad_setup_program_done_ && !controller_program_active_)
                keypad_setup_phase_ = KeypadSetupPhase::Verify;
            break;
        case KeypadSetupPhase::Verify:
            keypad_setup_status_[keypad_setup_target_] = "verifying_all_assignments";
            keypad_setup_emit();
            start_mapping_read(KeypadSetupPhase::WaitVerify);
            break;
        case KeypadSetupPhase::WaitVerify: {
            if (!keypad_setup_mapping_done_)
                break;
            const auto *snapshot = controller_bindings_.find(keypad_setup_target_);
            auto expected = keypad_setup_working_;
            expected[keypad_setup_slot_ - 1] = keypad_setup_plan_.after[keypad_setup_slot_ - 1];
            if (!keypad_setup_mapping_ok_ || !snapshot || !snapshot->valid ||
                snapshot->records != expected ||
                binding_refresh_counts_[0][3] != keypad_assigned_count(expected)) {
                finish_keypad_setup("uncertain_program_outcome_manual_reconciliation_required");
                break;
            }
            if (!keypad_setup_journal_.mark_verified(keypad_setup_slot_)) {
                finish_keypad_setup("journal_save_failed");
                break;
            }
            keypad_setup_working_ = expected;
            keypad_setup_phase_ = KeypadSetupPhase::SlotRead;
            break;
        }
        case KeypadSetupPhase::Finish: {
            auto *dev = db_.find_device(keypad_setup_target_);
            if (!dev) {
                finish_keypad_setup("device_removed");
                break;
            }
            dev->ha_setup_pending = false;
            if (!db_.save()) {
                dev->ha_setup_pending = true;
                finish_keypad_setup("save_failed");
                break;
            }
            if (keypad_setup_journal_.active && !keypad_setup_journal_.clear()) {
                finish_keypad_setup("journal_clear_failed");
                break;
            }
            publish_all_discovery();
            subscribe_all_commands();
            finish_keypad_setup(keypad_setup_plan_.needs_review ? "existing_assignments_need_review"
                                                                : "ready");
            break;
        }
    }
}
}  // namespace avionmesh
