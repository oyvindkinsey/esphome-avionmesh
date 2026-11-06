#include "controller_bindings.h"
#ifdef USE_ESP32
#include <nvs.h>
#endif

namespace avionmesh {
std::vector<uint8_t> ControllerBindings::encode() const {
    std::vector<uint8_t> blob{1, static_cast<uint8_t>(snapshots_.size())};
    for (unsigned i = 0; i < 8; ++i)
        blob.push_back(mesh_tag_ >> (i * 8));
    for (const auto &s : snapshots_) {
        blob.push_back(s.controller & 255);
        blob.push_back(s.controller >> 8);
        blob.push_back(s.product);
        blob.push_back(s.valid ? 1 : 0);
        for (const auto &r : s.records)
            blob.insert(blob.end(), r.begin(), r.end());
    }
    return blob;
}
bool ControllerBindings::decode(const std::vector<uint8_t> &blob, const std::string &mesh_key) {
    snapshots_.clear();
    mesh_tag_ = controller_mesh_tag(mesh_key);
    if (blob.size() < 10 || blob[0] != 1 || blob[1] > MAX_CONTROLLERS ||
        blob.size() != 10u + blob[1] * 36u)
        return false;
    uint64_t tag = 0;
    for (unsigned i = 0; i < 8; ++i)
        tag |= uint64_t(blob[2 + i]) << (i * 8);
    if (tag != mesh_tag_)
        return false;
    std::vector<ControllerBindingSnapshot> parsed;
    for (size_t pos = 10; pos < blob.size(); pos += 36) {
        ControllerBindingSnapshot s;
        s.controller = static_cast<uint16_t>(blob[pos]) | static_cast<uint16_t>(blob[pos + 1]) << 8;
        s.product = blob[pos + 2];
        s.valid = blob[pos + 3] == 1;
        if (s.controller < 32896 || s.controller > 65407 ||
            (s.product != 127 && s.product != 126) || blob[pos + 3] > 1)
            return false;
        for (unsigned slot = 0; slot < 4; ++slot)
            std::copy_n(blob.begin() + pos + 4 + slot * 8, 8, s.records[slot].begin());
        if (!valid_binding_records(s.records) ||
            std::any_of(parsed.begin(), parsed.end(), [&](const ControllerBindingSnapshot &old) {
                return old.controller == s.controller;
            }))
            return false;
        parsed.push_back(s);
    }
    snapshots_ = std::move(parsed);
    return true;
}
void ControllerBindings::load(const std::string &mesh_key) {
    mesh_tag_ = controller_mesh_tag(mesh_key);
    snapshots_.clear();
#ifdef USE_ESP32
    nvs_handle_t handle;
    if (nvs_open("avion_buttons", NVS_READONLY, &handle) != ESP_OK)
        return;
    size_t size = 0;
    if (nvs_get_blob(handle, "bindings", nullptr, &size) == ESP_OK &&
        size <= 10 + MAX_CONTROLLERS * 36) {
        std::vector<uint8_t> blob(size);
        if (nvs_get_blob(handle, "bindings", blob.data(), &size) == ESP_OK)
            decode(blob, mesh_key);
    }
    nvs_close(handle);
#endif
}
bool ControllerBindings::save() const {
#ifdef USE_ESP32
    nvs_handle_t handle;
    if (nvs_open("avion_buttons", NVS_READWRITE, &handle) != ESP_OK)
        return false;
    const auto blob = encode();
    bool ok = nvs_set_blob(handle, "bindings", blob.data(), blob.size()) == ESP_OK;
    if (ok)
        ok = nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
#else
    return true;
#endif
}
bool ControllerBindings::bind_mesh(const std::string &mesh_key) {
    const auto tag = controller_mesh_tag(mesh_key);
    if (tag == mesh_tag_)
        return false;
    mesh_tag_ = tag;
    snapshots_.clear();
    save();
    return true;
}
bool ControllerBindings::set(uint16_t controller, uint8_t product,
                             const ControllerSlotRecords &records) {
    if (controller < 32896 || controller > 65407 || (product != 127 && product != 126) ||
        !valid_binding_records(records))
        return false;
    const auto old = snapshots_;
    for (auto &s : snapshots_)
        if (s.controller == controller) {
            s = {controller, product, true, records};
            if (save())
                return true;
            snapshots_ = old;
            return false;
        }
    if (snapshots_.size() >= MAX_CONTROLLERS)
        return false;
    snapshots_.push_back({controller, product, true, records});
    if (save())
        return true;
    snapshots_ = old;
    return false;
}
bool ControllerBindings::invalidate(uint16_t controller) {
    for (auto &s : snapshots_)
        if (s.controller == controller && s.valid) {
            s.valid = false;
            save();
            return true;
        }
    return false;
}
void ControllerBindings::invalidate_all() {
    bool changed = false;
    for (auto &s : snapshots_)
        if (s.valid) {
            s.valid = false;
            changed = true;
        }
    if (changed)
        save();
}
void ControllerBindings::remove(uint16_t controller) {
    auto end = std::remove_if(
        snapshots_.begin(), snapshots_.end(),
        [&](const ControllerBindingSnapshot &s) { return s.controller == controller; });
    if (end != snapshots_.end()) {
        snapshots_.erase(end, snapshots_.end());
        save();
    }
}
}  // namespace avionmesh
