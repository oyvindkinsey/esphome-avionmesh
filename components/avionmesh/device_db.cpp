#include "device_db.h"

#ifdef USE_ESP32
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_system.h>
#include <mbedtls/base64.h>
#endif

#include <algorithm>
#include <cstring>
#include <ctime>
#include "esphome/core/helpers.h"

namespace avionmesh {

#ifdef USE_ESP32
static const char *NVS_NAMESPACE = "avionmesh";
static const char *NVS_KEY_DEVICES = "devices";
static const char *NVS_KEY_GROUPS = "groups";
static const char *NVS_KEY_PASSPHRASE = "passphrase";

/*
 * NVS storage format (compact binary):
 * Devices: [count(2)] [id(2) product(1) name_len(1) name(...) group_count(2) groups(2*N)]...
 * Groups:  [count(2)] [id(2) name_len(1) name(...) member_count(2) members(2*N)]...
 */
#endif

void DeviceDB::load() {
#ifdef USE_ESP32
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
        return;

    /* Load passphrase */
    size_t len = 0;
    if (nvs_get_str(handle, NVS_KEY_PASSPHRASE, nullptr, &len) == ESP_OK && len > 0) {
        passphrase_.resize(len - 1);
        nvs_get_str(handle, NVS_KEY_PASSPHRASE, &passphrase_[0], &len);
    }

    size_t blob_size = 0;
    if (nvs_get_blob(handle, NVS_KEY_DEVICES, nullptr, &blob_size) == ESP_OK && blob_size > 2) {
        std::vector<uint8_t> buf(blob_size);
        nvs_get_blob(handle, NVS_KEY_DEVICES, buf.data(), &blob_size);

        size_t pos = 0;
        uint16_t count = buf[pos] | (buf[pos + 1] << 8); pos += 2;
        for (uint16_t i = 0; i < count && pos < blob_size; i++) {
            DeviceEntry d;
            d.avion_id = buf[pos] | (buf[pos + 1] << 8); pos += 2;
            d.product_type = buf[pos++];
            uint8_t name_len = buf[pos++];
            d.name.assign(reinterpret_cast<char *>(&buf[pos]), name_len); pos += name_len;
            uint16_t gc = buf[pos] | (buf[pos + 1] << 8); pos += 2;
            for (uint16_t g = 0; g < gc && pos + 1 < blob_size; g++) {
                d.groups.push_back(buf[pos] | (buf[pos + 1] << 8)); pos += 2;
            }
            devices_.push_back(std::move(d));
        }
    }

    blob_size = 0;
    if (nvs_get_blob(handle, NVS_KEY_GROUPS, nullptr, &blob_size) == ESP_OK && blob_size > 2) {
        std::vector<uint8_t> buf(blob_size);
        nvs_get_blob(handle, NVS_KEY_GROUPS, buf.data(), &blob_size);

        size_t pos = 0;
        uint16_t count = buf[pos] | (buf[pos + 1] << 8); pos += 2;
        for (uint16_t i = 0; i < count && pos < blob_size; i++) {
            GroupEntry g;
            g.group_id = buf[pos] | (buf[pos + 1] << 8); pos += 2;
            uint8_t name_len = buf[pos++];
            g.name.assign(reinterpret_cast<char *>(&buf[pos]), name_len); pos += name_len;
            uint16_t mc = buf[pos] | (buf[pos + 1] << 8); pos += 2;
            for (uint16_t m = 0; m < mc && pos + 1 < blob_size; m++) {
                g.member_ids.push_back(buf[pos] | (buf[pos + 1] << 8)); pos += 2;
            }
            groups_.push_back(std::move(g));
        }
    }

    nvs_close(handle);
#endif
}

void DeviceDB::save() {
#ifdef USE_ESP32
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
        return;

    /* Save passphrase */
    if (!passphrase_.empty())
        nvs_set_str(handle, NVS_KEY_PASSPHRASE, passphrase_.c_str());
    else
        nvs_erase_key(handle, NVS_KEY_PASSPHRASE);

    /* Serialize devices */
    std::vector<uint8_t> buf;
    uint16_t count = devices_.size();
    buf.push_back(count & 0xFF); buf.push_back(count >> 8);
    for (auto &d : devices_) {
        buf.push_back(d.avion_id & 0xFF); buf.push_back(d.avion_id >> 8);
        buf.push_back(d.product_type);
        buf.push_back(static_cast<uint8_t>(d.name.size()));
        buf.insert(buf.end(), d.name.begin(), d.name.end());
        uint16_t gc = d.groups.size();
        buf.push_back(gc & 0xFF); buf.push_back(gc >> 8);
        for (auto gid : d.groups) {
            buf.push_back(gid & 0xFF); buf.push_back(gid >> 8);
        }
    }
    nvs_set_blob(handle, NVS_KEY_DEVICES, buf.data(), buf.size());

    /* Serialize groups */
    buf.clear();
    count = groups_.size();
    buf.push_back(count & 0xFF); buf.push_back(count >> 8);
    for (auto &g : groups_) {
        buf.push_back(g.group_id & 0xFF); buf.push_back(g.group_id >> 8);
        buf.push_back(static_cast<uint8_t>(g.name.size()));
        buf.insert(buf.end(), g.name.begin(), g.name.end());
        uint16_t mc = g.member_ids.size();
        buf.push_back(mc & 0xFF); buf.push_back(mc >> 8);
        for (auto mid : g.member_ids) {
            buf.push_back(mid & 0xFF); buf.push_back(mid >> 8);
        }
    }
    nvs_set_blob(handle, NVS_KEY_GROUPS, buf.data(), buf.size());

    nvs_commit(handle);
    nvs_close(handle);
#endif
}

bool DeviceDB::add_device(uint16_t avion_id, uint8_t product_type, const std::string &name) {
    if (find_device(avion_id))
        return false;
    devices_.push_back({avion_id, product_type, name, {}});
    save();
    return true;
}

bool DeviceDB::remove_device(uint16_t avion_id) {
    auto it = std::remove_if(devices_.begin(), devices_.end(),
                              [avion_id](const DeviceEntry &d) { return d.avion_id == avion_id; });
    if (it == devices_.end())
        return false;
    devices_.erase(it, devices_.end());

    for (auto &g : groups_) {
        g.member_ids.erase(
            std::remove(g.member_ids.begin(), g.member_ids.end(), avion_id),
            g.member_ids.end());
    }
    save();
    return true;
}

DeviceEntry *DeviceDB::find_device(uint16_t avion_id) {
    for (auto &d : devices_)
        if (d.avion_id == avion_id)
            return &d;
    return nullptr;
}

bool DeviceDB::add_group(uint16_t group_id, const std::string &name) {
    if (find_group(group_id))
        return false;
    groups_.push_back({group_id, name, {}});
    save();
    return true;
}

bool DeviceDB::remove_group(uint16_t group_id) {
    auto it = std::remove_if(groups_.begin(), groups_.end(),
                              [group_id](const GroupEntry &g) { return g.group_id == group_id; });
    if (it == groups_.end())
        return false;
    groups_.erase(it, groups_.end());

    for (auto &d : devices_) {
        d.groups.erase(
            std::remove(d.groups.begin(), d.groups.end(), group_id),
            d.groups.end());
    }
    save();
    return true;
}

GroupEntry *DeviceDB::find_group(uint16_t group_id) {
    for (auto &g : groups_)
        if (g.group_id == group_id)
            return &g;
    return nullptr;
}

bool DeviceDB::add_device_to_group(uint16_t avion_id, uint16_t group_id) {
    auto *dev = find_device(avion_id);
    auto *grp = find_group(group_id);
    if (!dev || !grp)
        return false;

    if (std::find(dev->groups.begin(), dev->groups.end(), group_id) == dev->groups.end())
        dev->groups.push_back(group_id);
    if (std::find(grp->member_ids.begin(), grp->member_ids.end(), avion_id) == grp->member_ids.end())
        grp->member_ids.push_back(avion_id);

    save();
    return true;
}

bool DeviceDB::remove_device_from_group(uint16_t avion_id, uint16_t group_id) {
    auto *dev = find_device(avion_id);
    auto *grp = find_group(group_id);
    if (!dev || !grp)
        return false;

    dev->groups.erase(std::remove(dev->groups.begin(), dev->groups.end(), group_id),
                      dev->groups.end());
    grp->member_ids.erase(std::remove(grp->member_ids.begin(), grp->member_ids.end(), avion_id),
                           grp->member_ids.end());
    save();
    return true;
}

void DeviceDB::clear() {
#ifdef USE_ESP32
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
        return;

    nvs_erase_key(handle, NVS_KEY_DEVICES);
    nvs_erase_key(handle, NVS_KEY_GROUPS);
    nvs_erase_key(handle, NVS_KEY_PASSPHRASE);
    nvs_commit(handle);
    nvs_close(handle);
#endif
    devices_.clear();
    groups_.clear();
    passphrase_.clear();
}

void DeviceDB::set_passphrase(const std::string &passphrase) {
    passphrase_ = passphrase;
    save();
}

void DeviceDB::generate_passphrase() {
    /* Generate 16 random bytes and encode as base64 */
    uint8_t raw[16];
    for (size_t i = 0; i < sizeof(raw); i += 4) {
        uint32_t rand_val = esphome::random_uint32();
        std::memcpy(raw + i, &rand_val, 4);
    }

    /* Base64 encode: 16 bytes -> 24 chars + padding = 24 chars */
    char b64[32];
    size_t out_len = 0;
    mbedtls_base64_encode(reinterpret_cast<unsigned char *>(b64), sizeof(b64),
                          &out_len, raw, sizeof(raw));

    passphrase_.assign(b64, out_len);
    save();
}

}  // namespace avionmesh
