#include "device_db.h"

#ifdef USE_ESP32
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_system.h>
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
 * Devices: [count(2)] [id(2) product(1) flags(1) name_len(1) name(...) group_count(2)
 * groups(2*N)]... Groups:  [count(2)] [id(2) flags(1) name_len(1) name(...) member_count(2)
 * members(2*N)]...
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
        uint16_t count = buf[pos] | (buf[pos + 1] << 8);
        pos += 2;
        for (uint16_t i = 0; i < count && pos < blob_size; i++) {
            DeviceEntry d;
            d.avion_id = buf[pos] | (buf[pos + 1] << 8);
            pos += 2;
            d.product_type = buf[pos++];
            uint8_t flags = buf[pos++];
            d.mqtt_exposed = flags & 0x01;
            d.ha_setup_pending = flags & 0x02;
            d.ha_local_level = d.product_type == 91 && (flags & 0x04);
            uint8_t name_len = buf[pos++];
            d.name.assign(reinterpret_cast<char *>(&buf[pos]), name_len);
            pos += name_len;
            uint16_t gc = buf[pos] | (buf[pos + 1] << 8);
            pos += 2;
            for (uint16_t g = 0; g < gc && pos + 1 < blob_size; g++) {
                d.groups.push_back(buf[pos] | (buf[pos + 1] << 8));
                pos += 2;
            }
            devices_.push_back(std::move(d));
        }
    }

    blob_size = 0;
    if (nvs_get_blob(handle, NVS_KEY_GROUPS, nullptr, &blob_size) == ESP_OK && blob_size > 2) {
        std::vector<uint8_t> buf(blob_size);
        nvs_get_blob(handle, NVS_KEY_GROUPS, buf.data(), &blob_size);

        size_t pos = 0;
        uint16_t count = buf[pos] | (buf[pos + 1] << 8);
        pos += 2;
        for (uint16_t i = 0; i < count && pos < blob_size; i++) {
            GroupEntry g;
            g.group_id = buf[pos] | (buf[pos + 1] << 8);
            pos += 2;
            uint8_t gflags = buf[pos++];
            g.mqtt_exposed = gflags & 0x01;
            g.ha_event_target = gflags & 0x02;
            uint8_t name_len = buf[pos++];
            g.name.assign(reinterpret_cast<char *>(&buf[pos]), name_len);
            pos += name_len;
            uint16_t mc = buf[pos] | (buf[pos + 1] << 8);
            pos += 2;
            for (uint16_t m = 0; m < mc && pos + 1 < blob_size; m++) {
                g.member_ids.push_back(buf[pos] | (buf[pos + 1] << 8));
                pos += 2;
            }
            groups_.push_back(std::move(g));
        }
    }

    nvs_close(handle);
#endif
}

void DeviceDB::begin_batch() {
    batch_mode_ = true;
}

void DeviceDB::end_batch() {
    batch_mode_ = false;
    save();
}

bool DeviceDB::save() {
    if (batch_mode_)
        return true;  // Queued in-memory; end_batch() performs the single commit.
#ifdef USE_ESP32
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
        return false;

    /* Save passphrase */
    esp_err_t key_result = !passphrase_.empty()
                               ? nvs_set_str(handle, NVS_KEY_PASSPHRASE, passphrase_.c_str())
                               : nvs_erase_key(handle, NVS_KEY_PASSPHRASE);
    bool ok = key_result == ESP_OK || key_result == ESP_ERR_NVS_NOT_FOUND;

    /* Serialize devices */
    std::vector<uint8_t> buf;
    uint16_t count = devices_.size();
    buf.push_back(count & 0xFF);
    buf.push_back(count >> 8);
    for (auto &d : devices_) {
        buf.push_back(d.avion_id & 0xFF);
        buf.push_back(d.avion_id >> 8);
        buf.push_back(d.product_type);
        buf.push_back((d.mqtt_exposed ? 0x01 : 0x00) | (d.ha_setup_pending ? 0x02 : 0x00) |
                      (d.ha_local_level ? 0x04 : 0x00));
        buf.push_back(static_cast<uint8_t>(d.name.size()));
        buf.insert(buf.end(), d.name.begin(), d.name.end());
        uint16_t gc = d.groups.size();
        buf.push_back(gc & 0xFF);
        buf.push_back(gc >> 8);
        for (auto gid : d.groups) {
            buf.push_back(gid & 0xFF);
            buf.push_back(gid >> 8);
        }
    }
    ok = nvs_set_blob(handle, NVS_KEY_DEVICES, buf.data(), buf.size()) == ESP_OK && ok;

    /* Serialize groups */
    buf.clear();
    count = groups_.size();
    buf.push_back(count & 0xFF);
    buf.push_back(count >> 8);
    for (auto &g : groups_) {
        buf.push_back(g.group_id & 0xFF);
        buf.push_back(g.group_id >> 8);
        buf.push_back((g.mqtt_exposed ? 0x01 : 0x00) | (g.ha_event_target ? 0x02 : 0x00));
        buf.push_back(static_cast<uint8_t>(g.name.size()));
        buf.insert(buf.end(), g.name.begin(), g.name.end());
        uint16_t mc = g.member_ids.size();
        buf.push_back(mc & 0xFF);
        buf.push_back(mc >> 8);
        for (auto mid : g.member_ids) {
            buf.push_back(mid & 0xFF);
            buf.push_back(mid >> 8);
        }
    }
    ok = nvs_set_blob(handle, NVS_KEY_GROUPS, buf.data(), buf.size()) == ESP_OK && ok;

    if (ok)
        ok = nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
#else
    return true;
#endif
}

bool DeviceDB::replace_inventory_metadata(std::vector<DeviceEntry> devices,
                                          std::vector<GroupEntry> groups) {
    DeviceDB candidate;
    candidate.passphrase_ = passphrase_;
    candidate.devices_ = std::move(devices);
    candidate.groups_ = std::move(groups);
    if (!candidate.save())
        return false;
    devices_.swap(candidate.devices_);
    groups_.swap(candidate.groups_);
    return true;
}

bool DeviceDB::add_device(uint16_t avion_id, uint8_t product_type, const std::string &name,
                          bool local_level) {
    if (find_device(avion_id))
        return false;
    devices_.push_back({avion_id,
                        product_type,
                        name,
                        {},
                        controller_ha_default(product_type),
                        keypad_setup_default(product_type),
                        product_type == 91 && local_level});
    save();
    return true;
}

const DeviceEntry *DeviceDB::find_device(uint16_t avion_id) const {
    for (const auto &device : devices_)
        if (device.avion_id == avion_id)
            return &device;
    return nullptr;
}

bool DeviceDB::remove_device(uint16_t avion_id) {
    auto it = std::remove_if(devices_.begin(), devices_.end(),
                             [avion_id](const DeviceEntry &d) { return d.avion_id == avion_id; });
    if (it == devices_.end())
        return false;
    devices_.erase(it, devices_.end());

    for (auto &g : groups_) {
        g.member_ids.erase(std::remove(g.member_ids.begin(), g.member_ids.end(), avion_id),
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

bool DeviceDB::rename_device(uint16_t avion_id, const std::string &name) {
    if (name.empty())
        return false;
    auto *dev = find_device(avion_id);
    if (!dev)
        return false;
    dev->name = name;
    save();
    return true;
}

bool DeviceDB::rename_group(uint16_t group_id, const std::string &name) {
    if (name.empty())
        return false;
    auto *grp = find_group(group_id);
    // Owned event target names encode their controller/slot. Renaming would
    // break ownership and allow later setup to allocate a second target.
    if (!grp || grp->ha_event_target || grp->name.rfind("HA event target: ", 0) == 0)
        return false;
    grp->name = name;
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
        d.groups.erase(std::remove(d.groups.begin(), d.groups.end(), group_id), d.groups.end());
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
    if (!dev || !grp || grp->ha_event_target)
        return false;

    if (std::find(dev->groups.begin(), dev->groups.end(), group_id) == dev->groups.end())
        dev->groups.push_back(group_id);
    if (std::find(grp->member_ids.begin(), grp->member_ids.end(), avion_id) ==
        grp->member_ids.end())
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

namespace {
// Standard base64 (RFC 4648) with padding; local so this does not depend on
// Mbed TLS utility APIs removed in Mbed TLS 4.
size_t base64_encode(const uint8_t *in, size_t len, char *out, size_t out_cap) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = 4 * ((len + 2) / 3);
    if (out_cap < need + 1)
        return 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = static_cast<uint32_t>(in[i]) << 16;
        if (i + 1 < len)
            v |= static_cast<uint32_t>(in[i + 1]) << 8;
        if (i + 2 < len)
            v |= in[i + 2];
        out[o++] = tbl[(v >> 18) & 0x3F];
        out[o++] = tbl[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < len) ? tbl[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < len) ? tbl[v & 0x3F] : '=';
    }
    out[o] = '\0';
    return o;
}
}  // namespace

void DeviceDB::generate_passphrase() {
#ifdef USE_ESP32
    /* Generate 16 random bytes and encode as base64 */
    uint8_t raw[16];
    for (size_t i = 0; i < sizeof(raw); i += 4) {
        uint32_t rand_val = esphome::random_uint32();
        std::memcpy(raw + i, &rand_val, 4);
    }

    /* Base64 encode: 16 bytes -> 24 chars */
    char b64[32];
    size_t out_len = base64_encode(raw, sizeof(raw), b64, sizeof(b64));

    passphrase_.assign(b64, out_len);
    save();
#endif
}

}  // namespace avionmesh
