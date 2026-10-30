#include "controller_profile.h"
#include "controller_events.h"
#include "local_dimmer.h"
#include <cassert>
#include <cstring>
using namespace avionmesh;
int main() {
    for (unsigned product = 0; product < 256; ++product) {
        const auto profile = dimmer_profile(product);
        assert(profile.supported == (product == 81 || product == 91));
        assert(profile.absolute_brightness == profile.supported);
        assert(profile.local_level_reports == (product == 91));
        assert(!profile.native_assignment_programming);
        for (bool configured : {false, true}) {
            assert(uses_local_dimmer_reports(product, configured) == (configured && product == 91));
            for (bool exposed : {false, true})
                for (bool baseline : {false, true}) {
                    const char *expected = !profile.supported ? "unsupported" : !exposed ? "disabled"
                        : configured && product == 91 ? (baseline ? "ready_local_reports" : "waiting_for_baseline")
                        : "native_commands";
                    assert(!std::strcmp(dimmer_input_status(product, exposed, configured, baseline), expected));
                }
        }
    }
    // A native absolute command is usable as the first input after reconnect.
    // A local status-shaped packet needs a silent baseline. Neither proves the
    // device's assignment state, and selecting one decoder never enables both.
    uint8_t native[] = {0, 10, 1, 16, 1, 0, 255, 0, 0, 0};
    uint8_t local[] = {1, 10, 0, 255};
    ControllerCommand command;
    assert(decode_controller_command(91, 0x73, native, sizeof(native), command) && command.absolute);
    AbsoluteDimmerDirection direction;
    assert(direction.observe(command.target, 10, command.brightness, 1000).accepted);
    assert(!decode_local_dimmer_report(91, 0x73, native, sizeof(native), command));
    assert(!decode_controller_command(91, 0x73, local, sizeof(local), command));
    assert(decode_local_dimmer_report(91, 0x73, local, sizeof(local), command));
    LocalDimmerReports reports;
    assert(!reports.observe(10, command.brightness, 1000).accepted && reports.ready());
    assert(reports.observe(11, 255, 1100).accepted);  // Repeated endpoint after baseline.
    reports = {};  // Same reset performed on MQTT/mesh reconnection and mode changes.
    assert(!reports.ready() && !reports.observe(12, 0, 2000).accepted);
    assert(reports.observe(13, 0, 2100).accepted);
}
