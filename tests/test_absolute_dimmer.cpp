#include "absolute_dimmer.h"
#include <cassert>
#include <cstring>
using namespace avionmesh;
int main() {
    assert(supports_absolute_dimmer_direction(81) && supports_absolute_dimmer_direction(91));
    assert(!supports_absolute_dimmer_direction(126) && !supports_absolute_dimmer_direction(127));
    assert(!supports_absolute_dimmer_direction(93));
    AbsoluteDimmerDirection d;
    auto first = d.observe(1201, 78830, 255, 1000);
    assert(!first.action &&
           first.accepted);  // Initial slider level is available without guessing direction.
    auto lower = d.observe(1201, 78831, 254, 1100);
    assert(lower.action && !std::strcmp(lower.action, "lower") && lower.difference == -1);
    assert(!d.observe(1201, 78831, 254, 1110).accepted);  // Duplicate.
    lower = d.observe(1201, 78835, 241, 1200);
    assert(lower.action && lower.difference == -13);
    assert(!d.observe(1201, 78833, 248, 1210)
                .accepted);  // Late higher level must not reverse direction or slider level.
    auto raise = d.observe(1201, 78836, 245, 1250);
    assert(raise.action && !std::strcmp(raise.action, "raise") && raise.previous == 241 &&
           raise.difference == 4);
    assert(!d.observe(1201, 78837, 255, 1300)
                .action);  // Full-on keeps its existing command semantics.
    assert(!d.observe(1201, 78838, 0, 1400).action);  // Off is not Lower.
    assert(
        !d.observe(1201, 78839, 128, 1500).action);  // No inference from Off to a restored level.
    assert(d.observe(1201, 78840, 132, 1600).action);
    assert(!d.observe(1201, 78841, 100, 4000).action);  // Idle gap.
    assert(d.observe(1201, 78842, 97, 4100).action);
    assert(!d.observe(1200, 78843, 120, 4200).action);  // Different target.
    assert(!d.observe(1200, 80000, 100, 4300).action);  // Large sequence gap.
    assert(!d.observe(1200, 3, 90, 20000).action);  // Rebase after a long idle/controller restart.
    assert(d.observe(1200, 4, 87, 20100).action);
    AbsoluteDimmerDirection wrap;
    assert(!wrap.observe(1, 0xffffff, 100, 0xfffffff0).action);
    assert(wrap.observe(1, 0, 103, 10).action);  // 24-bit sequence and millisecond wrap.
    assert(!wrap.observe(1, 0xffffff, 100, 20).action);
    AbsoluteDimmerDirection another;
    assert(!another.observe(1201, 1, 110, 1000).action);  // No cross-controller baseline.
}
