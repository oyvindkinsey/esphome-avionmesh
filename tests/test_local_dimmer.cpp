#include "local_dimmer.h"
#include <cassert>
#include <cstring>
using namespace avionmesh;
int main() {
    ControllerCommand c;
    uint8_t report[] = {1, 10, 0, 255};
    assert(decode_local_dimmer_report(91, 0x73, report, 4, c));
    assert(c.absolute && c.brightness == 255 && c.target == 0 && !std::strcmp(c.event_type, "on"));
    // Product-162 endpoint echoes share the same bytes. Unknown/untrusted sources
    // are represented by product 0 after the caller's authenticated lookup fails.
    for (uint8_t product : {0, 81, 126, 127, 162, 255}) {
        for (uint8_t level : {0, 197, 255}) {
            report[3] = level;
            c.absolute = true;
            assert(!decode_local_dimmer_report(product, 0x73, report, 4, c));
            assert(!c.absolute && !c.relative && !c.brightness);
        }
    }
    assert(!decode_local_dimmer_report(91, 0x74, report, 4, c));
    assert(!decode_local_dimmer_report(91, 0x73, report, 3, c));
    report[2] = 0xa0;
    assert(decode_local_dimmer_report(91, 0x73, report, 4, c) && c.transaction == 0xa0);
    assert(local_dimmer_report_is_query(c.transaction, false, 0, 100));
    report[2] = 0;
    report[3] = 0;
    assert(decode_local_dimmer_report(91, 0x73, report, 4, c));
    assert(!std::strcmp(c.event_type, "off"));
    report[3] = 197;
    assert(decode_local_dimmer_report(91, 0x73, report, 4, c));
    assert(!std::strcmp(c.event_type, "brightness"));

    // Read-only startup baseline, followed by the captured four physical levels.
    LocalDimmerReports d;
    assert(!d.ready());
    assert(!d.observe(1, 0, 1000, true).accepted && d.ready() && d.level() == 0);
    auto change = d.observe(2, 255, 6000);
    assert(change.accepted && !change.action && change.previous == 0);
    change = d.observe(3, 197, 11343);
    assert(change.accepted && !std::strcmp(change.action, "lower") && change.difference == -58);
    change = d.observe(4, 255, 17230);
    assert(change.accepted && !std::strcmp(change.action, "raise") && change.difference == 58);
    change = d.observe(5, 0, 21897);
    assert(change.accepted && !change.action);
    // Polls, duplicates and delayed responses cannot retrigger unchanged levels.
    assert(!d.observe(6, 0, 24000, true).accepted);
    assert(!d.observe(6, 0, 24001).accepted);
    assert(!d.observe(4, 255, 24002).accepted && d.level() == 0);
    assert(!d.observe(7, 0, 100000, true).accepted);
    // A changed level after a long idle is usable, without claiming direction.
    change = d.observe(8, 128, 200000);
    assert(change.accepted && !change.action);
    assert(!d.observe(1, 100, 220000).accepted && d.level() == 100);  // Controller restart rebases.
    change = d.observe(2, 120, 225000);
    assert(change.accepted && !std::strcmp(change.action, "raise"));
    LocalDimmerReports wrap;
    assert(!wrap.observe(0xffffff, 120, 0xfffffff0u).accepted);
    assert(wrap.observe(0, 110, 10).accepted);
    assert(!wrap.observe(0xffffff, 120, 11).accepted);
    LocalDimmerReports other;
    assert(!other.observe(2, 255, 100).accepted);  // No cross-controller baseline.
    // Identical On/Off input reports with new sequences survive; polls never do.
    LocalDimmerReports repeated;
    assert(!repeated.observe(10, 0, 1000, true).accepted);
    assert(repeated.observe(11, 255, 2000).accepted);
    auto again = repeated.observe(12, 255, 3000);
    assert(again.accepted && !again.action);
    assert(!repeated.observe(13, 255, 4000, true).accepted);
    assert(repeated.observe(14, 0, 5000).accepted);
    assert(repeated.observe(15, 0, 6000).accepted);
    assert(!repeated.observe(15, 0, 6001).accepted);
    assert(!repeated.observe(16, 128, 7000, true).accepted && repeated.level() == 128);
    uint8_t query[] = {1, 10, 0, 0, 0, 0, 0, 0, 0, 0};
    assert(is_untagged_dimming_query(0x73, query, 10));
    query[4] = 0xd0;
    assert(!is_untagged_dimming_query(0x73, query, 10));
    assert(!is_untagged_dimming_query(0x73, query, 4));
    assert(!local_dimmer_report_is_query(0, false, 0, 100));
    assert(local_dimmer_report_is_query(0, true, 100, 2099));
    assert(!local_dimmer_report_is_query(0, true, 100, 2100));
    assert(local_dimmer_report_is_query(0, true, 0xfffffff0u, 10));
}
