#pragma once
#include <cstdint>

namespace avionmesh {
// Capabilities describe observed event formats, never current native assignments.
// In particular a selected local report path does not prove a controller is empty.
struct DimmerProfile {
    bool supported{false};
    bool local_level_reports{false};
    bool absolute_brightness{false};
    bool native_assignment_programming{false};
};

constexpr DimmerProfile dimmer_profile(uint8_t product) {
    if (product == 91)
        return {true, true, true, false};
    if (product == 81)
        return {true, false, true, false};
    return {};
}

constexpr bool uses_local_dimmer_reports(uint8_t product, bool configured) {
    return configured && dimmer_profile(product).local_level_reports;
}

inline const char *dimmer_input_status(uint8_t product, bool exposed, bool configured_local,
                                       bool baseline_ready) {
    if (!dimmer_profile(product).supported)
        return "unsupported";
    if (!exposed)
        return "disabled";
    if (uses_local_dimmer_reports(product, configured_local))
        return baseline_ready ? "ready_local_reports" : "waiting_for_baseline";
    return "native_commands";  // Selected decoder; not proof of an assignment or a live device.
}

inline const char *dimmer_input_mode(uint8_t product, bool configured_local) {
    if (!dimmer_profile(product).supported)
        return "unsupported";
    return uses_local_dimmer_reports(product, configured_local) ? "local_level_reports"
                                                                : "native_commands";
}
}  // namespace avionmesh
