#include "keypad_setup_journal.h"
#include <cassert>
using namespace avionmesh;
int main() {
    ControllerSlotRecords records{};
    for (unsigned i = 0; i < 4; ++i)
        records[i] = {{1, 0x30, 0, static_cast<uint8_t>(i + 1), 255, 255, 0, 0}};
    KeypadSetupPlan p;
    std::string error;
    assert(plan_unused_keypad_buttons(40001, records, {}, {}, {}, p, error));
    KeypadSetupJournal journal;
    assert(journal.begin(p, "mesh"));
    assert(journal.active && journal.attempted == 0 && journal.verified == 0);
    assert(journal.mark_attempt(1));
    assert(!journal.mark_attempt(1));  // Never automatically resend the same uncertain write.
    KeypadSetupJournal boot;
    assert(boot.decode(journal.encode(), "mesh"));
    assert(boot.active && boot.attempted == 1 && boot.verified == 0);
    assert(boot.reconciles(records));
    auto changed = records;
    changed[0] = p.after[0];
    assert(boot.reconciles(changed));
    changed[1] = p.after[1];
    assert(!boot.reconciles(changed));  // Unattempted external edit.
    changed = records;
    changed[3][4] = 1;
    changed[3][5] = 2;
    assert(!boot.reconciles(changed));
    assert(!boot.decode(journal.encode(), "another-mesh"));
    auto truncated = journal.encode();
    truncated.pop_back();
    assert(!boot.decode(truncated, "mesh"));
    assert(journal.mark_verified(1));
    assert(boot.decode(journal.encode(), "mesh") && boot.verified == 1);
    assert(!journal.begin(p, "mesh"));
    assert(journal.clear() && !journal.active);
    assert(controller_ha_default(127) && controller_ha_default(126) && controller_ha_default(81));
    assert(!controller_ha_default(93) && !controller_ha_default(134));
    assert(keypad_setup_default(127) && !keypad_setup_default(126));
    assert(!keypad_setup_default(81) && !keypad_setup_default(93));
}
