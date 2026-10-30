#pragma once

// Pure parsing/validation: no network, persistence or mesh programming.
#include "device_db.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <vector>

namespace avionmesh {
struct CloudDevice {
    uint16_t id;
    uint8_t product;
    std::string name, pid;
};
struct CloudGroup {
    uint16_t id;
    std::string name;
    std::vector<uint16_t> members;
};
struct CloudInventory {
    std::vector<CloudDevice> devices;
    std::vector<CloudGroup> groups;
};

inline bool cloud_name_valid(const std::string &name) {
    // Names must fit the NVS byte length and the existing SSE name emitter.
    return !name.empty() && name.size() <= 128 &&
           std::none_of(name.begin(), name.end(),
                        [](unsigned char c) { return c < 32 || c == '"' || c == '\\'; });
}
inline bool cloud_pid_valid(const std::string &pid) {
    return !pid.empty() && pid.size() <= 128 &&
           std::all_of(pid.begin(), pid.end(),
                       [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; });
}
inline bool cloud_resource_pid(JsonVariantConst value, std::string &pid) {
    // Avi-on locations use 64-bit numeric PIDs; device/group PIDs are usually
    // strings. Normalize either representation without narrowing or path syntax.
    if (value.is<const char *>())
        pid = value.as<std::string>();
    else if (value.is<uint64_t>() && value.as<uint64_t>() > 0)
        pid = std::to_string(value.as<uint64_t>());
    else {
        pid.clear();
        return false;
    }
    return cloud_pid_valid(pid);
}
inline bool cloud_id(JsonVariantConst value, uint16_t &id) {
    if (!value.is<unsigned int>())
        return false;
    const auto n = value.as<unsigned int>();
    if (!n || n > 65535)
        return false;
    id = static_cast<uint16_t>(n);
    return true;
}
inline std::string cloud_lower(std::string value) {
    for (auto &c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
inline bool cloud_parse_devices(JsonVariantConst root, CloudInventory &out, std::string &error) {
    auto records = root["abstract_devices"].as<JsonArrayConst>();
    if (records.isNull() || records.size() > 512) {
        error = "invalid_inventory";
        return false;
    }
    std::set<uint16_t> ids;
    std::set<std::string> pids;
    for (JsonObjectConst record : records) {
        std::string kind = record["type"] | "";
        if (kind != "device" && kind != "controller")
            continue;
        CloudDevice d{};
        d.name = record["name"] | "";
        const bool valid_pid = cloud_resource_pid(record["pid"], d.pid);
        if (!cloud_id(record["avid"], d.id) || !record["product_id"].is<unsigned int>() ||
            record["product_id"].as<unsigned int>() > 255 || !valid_pid ||
            !cloud_name_valid(d.name) || !ids.insert(d.id).second || !pids.insert(d.pid).second) {
            error = "invalid_device_or_name";
            return false;
        }
        d.product = record["product_id"].as<unsigned int>();
        out.devices.push_back(std::move(d));
    }
    if (out.devices.empty()) {
        error = "empty_inventory";
        return false;
    }
    return true;
}
inline bool cloud_parse_group(JsonVariantConst root, CloudInventory &out, std::string &error) {
    auto record = root["group"].as<JsonObjectConst>();
    CloudGroup g{};
    g.name = record["name"] | "";
    auto members = record["devices"].as<JsonArrayConst>();
    if (record.isNull() || !cloud_id(record["avid"], g.id) || g.id < 256 || g.id > 24575 ||
        !cloud_name_valid(g.name) || g.name.rfind("HA event target: ", 0) == 0 ||
        members.isNull() || members.size() > 512 || out.groups.size() >= 128) {
        error = "invalid_group_or_name";
        return false;
    }
    for (const auto &old : out.groups)
        if (old.id == g.id) {
            error = "duplicate_group_id";
            return false;
        }
    for (JsonVariantConst member : members) {
        uint16_t id = 0;
        std::string pid;
        if (member.is<const char *>())
            pid = member.as<std::string>();
        else if (member.is<JsonObjectConst>()) {
            cloud_id(member["avid"], id);
            cloud_resource_pid(member["pid"], pid);
        } else
            cloud_id(member, id);
        auto found = std::find_if(
            out.devices.begin(), out.devices.end(),
            [&](const CloudDevice &d) { return id ? d.id == id : !pid.empty() && d.pid == pid; });
        if (found == out.devices.end()) {
            error = "unresolved_group_member";
            return false;
        }
        if (std::find(g.members.begin(), g.members.end(), found->id) == g.members.end())
            g.members.push_back(found->id);
    }
    out.groups.push_back(std::move(g));
    return true;
}

inline bool cloud_validate_merge(const CloudInventory &cloud,
                                 const std::vector<DeviceEntry> &devices,
                                 const std::vector<GroupEntry> &groups, std::string &error) {
    // Validate the entire plan against the latest local DB before changing anything.
    std::set<uint16_t> ids;
    for (const auto &d : cloud.devices) {
        if (!ids.insert(d.id).second) {
            error = "duplicate_device_id";
            return false;
        }
        for (const auto &g : groups)
            if (d.id == g.group_id) {
                error = "device_group_collision";
                return false;
            }
        for (const auto &old : devices)
            if (old.avion_id == d.id &&
                (old.product_type != d.product || cloud_lower(old.name) != cloud_lower(d.name))) {
                error = "device_identity_changed";
                return false;
            }
    }
    std::set<uint16_t> gids;
    for (const auto &g : cloud.groups) {
        if (!gids.insert(g.id).second) {
            error = "duplicate_group_id";
            return false;
        }
        if (ids.count(g.id)) {
            error = "device_group_collision";
            return false;
        }
        for (const auto &d : devices)
            if (g.id == d.avion_id) {
                error = "device_group_collision";
                return false;
            }
        for (auto member : g.members)
            if (!ids.count(member)) {
                error = "unresolved_group_member";
                return false;
            }
        for (const auto &old : groups)
            if (old.group_id == g.id) {
                if (old.ha_event_target) {
                    if (old.name != g.name || !old.member_ids.empty() || old.mqtt_exposed) {
                        error = "managed_event_target_changed";
                        return false;
                    }
                    // The vendor UI required a bootstrap member. Physical membership
                    // was removed locally; cloud metadata must not recreate that link.
                    continue;
                }
                if (old.name.rfind("HA event target: ", 0) == 0) {
                    error = "reserved_ha_target_collision";
                    return false;
                }
                if (old.name != g.name ||
                    std::any_of(old.member_ids.begin(), old.member_ids.end(), [&](uint16_t id) {
                        return std::find(g.members.begin(), g.members.end(), id) == g.members.end();
                    })) {
                    error = "existing_group_changed";
                    return false;
                }
            }
    }
    return true;
}
}  // namespace avionmesh
