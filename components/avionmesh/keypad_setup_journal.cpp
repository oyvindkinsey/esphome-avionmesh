#include "keypad_setup_journal.h"
#ifdef USE_ESP32
#include <nvs.h>
#endif
namespace avionmesh {
std::vector<uint8_t> KeypadSetupJournal::encode() const {
    std::vector<uint8_t> b{1, plan.write_mask, attempted, verified};
    for (unsigned i = 0; i < 8; ++i)
        b.push_back(mesh_tag >> (8 * i));
    b.push_back(controller & 255);
    b.push_back(controller >> 8);
    for (const auto &records : {plan.before, plan.after})
        for (const auto &r : records)
            b.insert(b.end(), r.begin(), r.end());
    return b;
}
bool KeypadSetupJournal::decode(const std::vector<uint8_t> &b, const std::string &key) {
    active = false;
    if (b.size() != 78 || b[0] != 1 || !b[1] || b[1] > 15 || (b[2] & ~b[1]) || (b[3] & ~b[2]))
        return false;
    uint64_t tag = 0;
    for (unsigned i = 0; i < 8; ++i)
        tag |= uint64_t(b[4 + i]) << (8 * i);
    if (tag != controller_mesh_tag(key))
        return false;
    const uint16_t id = uint16_t(b[12]) | uint16_t(b[13]) << 8;
    if (id < 32896 || id > 65407)
        return false;
    KeypadSetupPlan parsed;
    parsed.controller = id;
    parsed.write_mask = b[1];
    for (unsigned i = 0; i < 4; ++i) {
        std::copy_n(b.begin() + 14 + i * 8, 8, parsed.before[i].begin());
        std::copy_n(b.begin() + 46 + i * 8, 8, parsed.after[i].begin());
        const bool changing = parsed.write_mask & (1u << i);
        if (changing) {
            const auto &old = parsed.before[i];
            const auto &desired = parsed.after[i];
            if (binding_target(old) != 65535 || old[6] || old[7] || binding_target(desired) < 256 ||
                binding_target(desired) > 24575 || desired[6] != 8 || desired[7] != 255)
                return false;
        } else if (parsed.before[i] != parsed.after[i])
            return false;
        parsed.targets[i] = binding_target(parsed.after[i]);
    }
    if (!valid_binding_records(parsed.before) || !valid_binding_records(parsed.after))
        return false;
    plan = std::move(parsed);
    controller = id;
    mesh_tag = tag;
    attempted = b[2];
    verified = b[3];
    active = true;
    return true;
}
void KeypadSetupJournal::load(const std::string &key) {
    active = false;
#ifdef USE_ESP32
    nvs_handle_t h;
    if (nvs_open("avion_buttons", NVS_READONLY, &h) != ESP_OK)
        return;
    size_t n = 0;
    if (nvs_get_blob(h, "setup_intent", nullptr, &n) == ESP_OK && n == 78) {
        std::vector<uint8_t> b(n);
        if (nvs_get_blob(h, "setup_intent", b.data(), &n) == ESP_OK)
            decode(b, key);
    }
    nvs_close(h);
#endif
    (void)key;
}
bool KeypadSetupJournal::save() const {
#ifdef USE_ESP32
    nvs_handle_t h;
    if (nvs_open("avion_buttons", NVS_READWRITE, &h) != ESP_OK)
        return false;
    auto b = encode();
    bool ok = nvs_set_blob(h, "setup_intent", b.data(), b.size()) == ESP_OK;
    if (ok)
        ok = nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
#else
    return true;
#endif
}
bool KeypadSetupJournal::begin(const KeypadSetupPlan &new_plan, const std::string &key) {
    if (active || !new_plan.write_mask)
        return false;
    plan = new_plan;
    controller = plan.controller;
    mesh_tag = controller_mesh_tag(key);
    attempted = verified = 0;
    if (!save())
        return false;
    active = true;
    return true;
}
bool KeypadSetupJournal::mark_attempt(uint8_t slot) {
    if (!active || slot < 1 || slot > 4 || !(plan.write_mask & (1u << (slot - 1))) ||
        (attempted & (1u << (slot - 1))))
        return false;
    const auto old = attempted;
    attempted |= 1u << (slot - 1);
    if (save())
        return true;
    attempted = old;
    return false;
}
bool KeypadSetupJournal::mark_verified(uint8_t slot) {
    if (!active || slot < 1 || slot > 4 || !(attempted & (1u << (slot - 1))))
        return false;
    const auto old = verified;
    verified |= 1u << (slot - 1);
    if (save())
        return true;
    verified = old;
    return false;
}
bool KeypadSetupJournal::clear() {
#ifdef USE_ESP32
    nvs_handle_t h;
    if (nvs_open("avion_buttons", NVS_READWRITE, &h) != ESP_OK)
        return false;
    auto result = nvs_erase_key(h, "setup_intent");
    bool ok = result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND;
    if (ok)
        ok = nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (!ok)
        return false;
#endif
    active = false;
    return true;
}
bool KeypadSetupJournal::reconciles(const ControllerSlotRecords &actual) const {
    if (!active || !valid_binding_records(actual))
        return false;
    for (unsigned i = 0; i < 4; ++i) {
        if (actual[i] == plan.before[i])
            continue;
        if (!(attempted & (1u << i)) || actual[i] != plan.after[i])
            return false;
    }
    return true;
}
}  // namespace avionmesh
