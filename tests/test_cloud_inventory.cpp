#include "cloud_inventory.h"
#include <cassert>
#include <iostream>
using namespace avionmesh;

int main() {
    JsonDocument doc;
    assert(!deserializeJson(doc, R"({"abstract_devices":[
      {"type":"device","avid":40010,"product_id":93,"name":"Example","pid":"light-1"},
      {"type":"controller","avid":40201,"product_id":81,"name":"Anywhere","pid":"remote-1"},
      {"type":"group","avid":1201,"pid":"group-1"}]})"));
    CloudInventory cloud;
    std::string error;
    assert(cloud_parse_devices(doc.as<JsonVariantConst>(), cloud, error));
    assert(cloud.devices.size() == 2 && cloud.devices[1].product == 81);
    assert(!deserializeJson(
        doc,
        R"({"group":{"avid":1201,"name":"Example group","devices":["light-1",{"pid":"remote-1"},40010]}})"));
    assert(cloud_parse_group(doc.as<JsonVariantConst>(), cloud, error));
    assert(cloud.groups[0].members.size() == 2);
    const std::vector<DeviceEntry> local{{40010, 93, "Example", {1201}, true},
                                         {40020, 134, "Example bulb", {}, true}};
    const std::vector<GroupEntry> groups{{1201, "Example group", {40010}, true},
                                         {24574, "HA event target: 40101:2", {}, false},
                                         {24573, "HA event target: 40101:3", {}, false}};
    assert(cloud_validate_merge(cloud, local, groups, error));
    auto managed = groups;
    managed[0].member_ids.clear();
    managed[0].mqtt_exposed = false;
    managed[0].ha_event_target = true;
    assert(cloud_validate_merge(cloud, local, managed,
                                error));  // Bootstrap member stays locally detached.
    managed[0].member_ids = {40010};
    assert(!cloud_validate_merge(cloud, local, managed, error) &&
           error == "managed_event_target_changed");
    managed[0].member_ids.clear();
    managed[0].mqtt_exposed = true;
    assert(!cloud_validate_merge(cloud, local, managed, error));
    // Validation is read-only and does not demand cloud copies of local-only items.
    assert(local[1].avion_id == 40020 && groups[1].member_ids.empty() && local[0].mqtt_exposed);
    auto changed = cloud;
    changed.devices[0].product = 134;
    assert(!cloud_validate_merge(changed, local, groups, error) &&
           error == "device_identity_changed");
    changed = cloud;
    changed.groups[0].members = {40201};
    assert(!cloud_validate_merge(changed, local, groups, error) &&
           error == "existing_group_changed");
    changed = cloud;
    changed.groups[0].id = 24574;
    assert(!cloud_validate_merge(changed, local, groups, error) &&
           error == "reserved_ha_target_collision");
    changed = cloud;
    changed.devices[1].id = 1201;
    assert(!cloud_validate_merge(changed, local, groups, error) &&
           error == "device_group_collision");
    changed = cloud;
    changed.groups.push_back(cloud.groups[0]);
    assert(!cloud_validate_merge(changed, local, groups, error) && error == "duplicate_group_id");
    changed = cloud;
    changed.groups[0].members.push_back(999);
    assert(!cloud_validate_merge(changed, local, groups, error) &&
           error == "unresolved_group_member");
    CloudInventory bad;
    assert(!deserializeJson(
        doc,
        R"({"abstract_devices":[{"type":"controller","avid":40201,"product_id":256,"name":"Remote","pid":"remote"}]})"));
    assert(!cloud_parse_devices(doc.as<JsonVariantConst>(), bad, error));
    assert(
        !deserializeJson(doc, R"({"group":{"avid":1200,"name":"Group","devices":["missing"]}})"));
    assert(!cloud_parse_group(doc.as<JsonVariantConst>(), cloud, error));
    assert(!cloud_name_valid("bad\"name") && !cloud_name_valid("bad\nname"));
    assert(!cloud_pid_valid("../../sessions") && !cloud_pid_valid("?token=bad"));
    std::string pid;
    assert(!deserializeJson(doc, R"({"pid":1234567890123})"));
    assert(cloud_resource_pid(doc["pid"], pid) && pid == "1234567890123");
    doc["pid"] = "location-1";
    assert(cloud_resource_pid(doc["pid"], pid) && pid == "location-1");
    for (auto invalid : {0, -1}) {
        doc["pid"] = invalid;
        assert(!cloud_resource_pid(doc["pid"], pid));
    }
    doc["pid"] = 1.5;
    assert(!cloud_resource_pid(doc["pid"], pid));
    std::cout << "Cloud inventory parsing and protected merge checks passed\n";
}
