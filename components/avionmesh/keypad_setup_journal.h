#pragma once
#include "keypad_setup.h"

namespace avionmesh {
class KeypadSetupJournal {
 public:
    bool active{false};
    uint64_t mesh_tag{0};
    uint16_t controller{0};
    uint8_t attempted{0}, verified{0};
    KeypadSetupPlan plan;
    void load(const std::string &key);
    bool begin(const KeypadSetupPlan &new_plan, const std::string &key);
    bool mark_attempt(uint8_t slot);
    bool mark_verified(uint8_t slot);
    bool clear();
    bool reconciles(const ControllerSlotRecords &actual) const;
    std::vector<uint8_t> encode() const;
    bool decode(const std::vector<uint8_t> &blob, const std::string &key);

 private:
    bool save() const;
};
}  // namespace avionmesh
