#pragma once
#include "controller_bindings.h"
#include "controller_tables.h"
#include "device_db.h"
#include <set>

namespace avionmesh {
struct KeypadSetupPlan {
    uint16_t controller{0};
    ControllerSlotRecords before{}, after{};
    std::array<uint16_t, 4> targets{};
    std::vector<GroupEntry> new_groups;
    uint8_t write_mask{0};
    bool needs_review{false};
};
inline uint8_t keypad_assigned_count(const ControllerSlotRecords &records) {
    uint8_t count = 0;
    for (const auto &r : records)
        if (binding_target(r) != 65535 || r[6] || r[7])
            ++count;
    return count;
}

inline bool plan_unused_keypad_buttons(uint16_t controller, const ControllerSlotRecords &records,
                                       const std::vector<DeviceEntry> &devices,
                                       const std::vector<GroupEntry> &groups,
                                       const std::set<uint16_t> &extra_used_ids,
                                       KeypadSetupPlan &plan, std::string &error) {
    if (controller < 32896 || controller > 65407 || !valid_binding_records(records)) {
        error = "invalid_keypad_snapshot";
        return false;
    }
    plan = {};
    plan.controller = controller;
    plan.before = plan.after = records;
    std::set<uint16_t> used = extra_used_ids;
    for (const auto &d : devices) {
        used.insert(d.avion_id);
        used.insert(d.groups.begin(), d.groups.end());
    }
    for (const auto &g : groups)
        used.insert(g.group_id);
    for (const auto &r : records)
        used.insert(binding_target(r));
    ControllerBindingSnapshot snapshot{controller, 127, true, records};
    for (uint8_t slot = 1; slot <= 4; ++slot) {
        const auto &r = records[slot - 1];
        const bool unused = binding_target(r) == 65535 && r[6] == 0 && r[7] == 0;
        if (!unused) {
            plan.targets[slot - 1] = binding_target(r);
            if (std::string(binding_slot_status(&snapshot, 127, slot)) != "ready")
                plan.needs_review = true;
            continue;  // Native and unknown assignments are never overwritten.
        }
        const auto owner = controller_event_target_name(controller, slot);
        uint16_t target = 0;
        for (const auto &g : groups)
            if (g.name == owner) {
                if (target || !g.member_ids.empty() || g.mqtt_exposed || g.group_id < 256 ||
                    g.group_id > 24575) {
                    error = "unsafe_or_duplicate_reserved_target";
                    return false;
                }
                target = g.group_id;
            }
        if (!target) {
            for (int id = 24575; id >= 256; --id)
                if (!used.count(static_cast<uint16_t>(id))) {
                    target = id;
                    break;
                }
            if (!target) {
                error = "no_free_group_ids";
                return false;
            }
            used.insert(target);
            plan.new_groups.push_back({target, owner, {}, false});
        }
        // Never bind two slots to the same target, including an occupied slot
        // whose group was absent from the gateway's cloud inventory.
        for (const auto &old : records)
            if (binding_target(old) == target) {
                error = "reserved_target_already_assigned";
                return false;
            }
        plan.targets[slot - 1] = target;
        plan.write_mask |= 1u << (slot - 1);
        auto &desired = plan.after[slot - 1];
        desired[4] = target >> 8;
        desired[5] = target & 255;
        desired[6] = 8;
        desired[7] = 255;
    }
    return true;
}
}  // namespace avionmesh
