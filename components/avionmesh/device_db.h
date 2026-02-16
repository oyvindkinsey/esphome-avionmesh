#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace avionmesh {

struct DeviceEntry {
    uint16_t avion_id;
    uint8_t product_type;
    std::string name;
    std::vector<uint16_t> groups;
    bool mqtt_exposed{false};
};

struct GroupEntry {
    uint16_t group_id;
    std::string name;
    std::vector<uint16_t> member_ids;
    bool mqtt_exposed{false};
};

class DeviceDB {
 public:
    void load();
    void save();
    void clear();

    bool add_device(uint16_t avion_id, uint8_t product_type, const std::string &name);
    bool remove_device(uint16_t avion_id);
    DeviceEntry *find_device(uint16_t avion_id);
    const std::vector<DeviceEntry> &devices() const { return devices_; }

    bool add_group(uint16_t group_id, const std::string &name);
    bool remove_group(uint16_t group_id);
    GroupEntry *find_group(uint16_t group_id);
    const std::vector<GroupEntry> &groups() const { return groups_; }

    bool add_device_to_group(uint16_t avion_id, uint16_t group_id);
    bool remove_device_from_group(uint16_t avion_id, uint16_t group_id);

    const std::string &passphrase() const { return passphrase_; }
    void set_passphrase(const std::string &passphrase);
    void generate_passphrase();

 protected:
    std::vector<DeviceEntry> devices_;
    std::vector<GroupEntry> groups_;
    std::string passphrase_;
};

}  // namespace avionmesh
