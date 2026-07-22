#pragma once

#include <cstdint>
#include <string_view>

namespace lengjing::app {

struct CoordinateProbeSettings {
    bool coordinateDecrypt = true;
    bool algorithmDecrypt = false;
};

constexpr bool IsAlgorithmCoordinateProbeEnabled(
    std::string_view value) noexcept {
    return value == "1";
}

constexpr CoordinateProbeSettings ResolveCoordinateProbeSettings(
    bool algorithmOnly) noexcept {
    return algorithmOnly
        ? CoordinateProbeSettings{false, true}
        : CoordinateProbeSettings{true, false};
}

constexpr int ResolveCoordinateProbeExitCode(
    bool algorithmOnly,
    int runtimeExitCode,
    std::uint64_t coordinateSuccesses,
    bool coordinateContextReady,
    bool coordinateEntryReady,
    std::uint64_t algorithmCoordinateSuccesses) noexcept {
    if (runtimeExitCode != 0) return runtimeExitCode;
    if (algorithmOnly) {
        return algorithmCoordinateSuccesses != 0 ? 0 : 11;
    }
    if (coordinateSuccesses != 0) return 0;
    return coordinateContextReady ? 13 : (coordinateEntryReady ? 12 : 11);
}

}  // namespace lengjing::app
