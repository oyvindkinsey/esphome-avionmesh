#include "keypad_setup.h"
#include <cassert>
using namespace avionmesh;
int main() {
    for (uint8_t product : {127}) {
        assert(queue_keypad_on_ha_enable(product, false, true));
        assert(!queue_keypad_on_ha_enable(product, true, true));
        assert(!queue_keypad_on_ha_enable(product, true, false));
        assert(!queue_keypad_on_ha_enable(product, false, false));
    }
    for (uint8_t product : {81, 91, 93, 126, 134})
        assert(!queue_keypad_on_ha_enable(product, false, true));
    ControllerSlotRecords native_group{{{{1, 0x30, 0, 1, 4, 0xb0, 8, 255}},
                                        {{1, 0x30, 0, 2, 0x5f, 0xfe, 8, 255}},
                                        {{1, 0x30, 0, 3, 0x5f, 0xfd, 8, 255}},
                                        {{1, 0x30, 0, 4, 0xff, 0xff, 0, 0}}}};
    std::vector<GroupEntry> groups{{1200, "Native group", {}, true},
                                   {24574, controller_event_target_name(40101, 2), {}, false},
                                   {24573, controller_event_target_name(40101, 3), {}, false}};
    KeypadSetupPlan p;
    std::string error;
    assert(keypad_assigned_count(native_group) == 3);
    assert(plan_unused_keypad_buttons(40101, native_group, {}, groups, {}, p, error));
    assert(p.write_mask == 8 && p.new_groups.size() == 1 && !p.needs_review);
    for (unsigned i = 0; i < 3; ++i)
        assert(p.after[i] == native_group[i]);
    assert(p.targets[3] == 24575 && p.new_groups[0].member_ids.empty() &&
           !p.new_groups[0].mqtt_exposed);
    auto reserved = groups;
    reserved.push_back({24572, controller_event_target_name(40101, 4), {}, false});
    assert(plan_unused_keypad_buttons(40101, native_group, {}, reserved, {}, p, error));
    assert(p.new_groups.empty() && p.targets[3] == 24572);
    reserved.back().member_ids = {40100};
    assert(!plan_unused_keypad_buttons(40101, native_group, {}, reserved, {}, p, error));
    ControllerSlotRecords blank = native_group;
    for (auto &r : blank) {
        r[4] = r[5] = 255;
        r[6] = r[7] = 0;
    }
    assert(plan_unused_keypad_buttons(40001, blank, {}, groups, {24575}, p, error));
    assert(p.write_mask == 15 && p.new_groups.size() == 4);
    std::set<uint16_t> targets(p.targets.begin(), p.targets.end());
    assert(targets.size() == 4 && !targets.count(24575));
    auto ambiguous = native_group;
    ambiguous[1][4] = 4;
    ambiguous[1][5] = 0xb0;
    assert(plan_unused_keypad_buttons(40101, ambiguous, {}, groups, {}, p, error));
    assert(p.needs_review && p.after[1] == ambiguous[1] && p.write_mask == 8);
    // Re-running setup after successful programming is idempotent.
    assert(plan_unused_keypad_buttons(40101, native_group, {}, groups, {}, p, error));
    auto finished = p.after;
    auto with_target = groups;
    with_target.insert(with_target.end(), p.new_groups.begin(), p.new_groups.end());
    assert(keypad_assigned_count(finished) == 4);
    assert(plan_unused_keypad_buttons(40101, finished, {}, with_target, {}, p, error));
    assert(!p.write_mask && p.new_groups.empty());
}
