#pragma once

#include "game/native/AlgorithmCoordinateDiagnostics.h"
#include "game/native/RuntimeCoordinateCodec.h"

#include <cstdint>

namespace lengjing::game::native {

inline bool IsAlgorithmCoordinateProbeSuccessful(
    bool requested,
    bool active,
    std::uint64_t successes,
    const AlgorithmCoordinateDiagnostic& successfulDiagnostic) noexcept {
    return requested && active && successes != 0 &&
        successfulDiagnostic.error == AlgorithmCoordinateReadError::None &&
        IsValidAlgorithmCoordinateValue(
            successfulDiagnostic.x,
            successfulDiagnostic.y,
            successfulDiagnostic.z);
}

inline bool IsAlgorithmCoordinateObjectProbeSuccessful(
    bool requested,
    bool active,
    std::uint64_t objectSuccesses,
    std::uint64_t standardFallbacks,
    const RuntimeCoordinateCodecDiagnostic& successfulDiagnostic) noexcept {
    return requested && active && objectSuccesses != 0 &&
        standardFallbacks == 0 &&
        successfulDiagnostic.error == RuntimeCoordinateCodecError::None &&
        successfulDiagnostic.stage ==
            RuntimeCoordinateCodecStage::RingDecoded &&
        IsValidAlgorithmCoordinateValue(
            successfulDiagnostic.decodedX,
            successfulDiagnostic.decodedY,
            successfulDiagnostic.decodedZ);
}

}  // namespace lengjing::game::native
