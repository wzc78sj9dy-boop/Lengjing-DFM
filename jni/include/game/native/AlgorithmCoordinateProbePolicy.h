#pragma once

#include "game/native/AlgorithmCoordinateDiagnostics.h"
#include "game/native/PositionReadModePolicy.h"
#include "game/native/RuntimeCoordinateCodec.h"

#include <cstdint>

namespace lengjing::game::native {

inline constexpr bool kAlgorithmCoordinateVisualAcceptanceCompleted = false;

inline bool IsAlgorithmCoordinateSampleValid(
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

inline bool IsAlgorithmCoordinateProbeSuccessful(
    bool requested,
    bool active,
    std::uint64_t successes,
    const AlgorithmCoordinateDiagnostic& successfulDiagnostic) noexcept {
    return kAlgorithmCoordinateVisualAcceptanceCompleted &&
        IsAlgorithmCoordinateSampleValid(
            requested, active, successes, successfulDiagnostic);
}

inline bool IsAlgorithmCoordinateObjectSampleValid(
    bool requested,
    bool active,
    std::uint64_t objectSuccesses,
    std::uint64_t standardFallbacks,
    const RuntimeCoordinateCodecDiagnostic& successfulDiagnostic) noexcept {
    const AlgorithmCoordinateFinalizeResult finalized =
        FinalizeAlgorithmCharacterCoordinate(
            successfulDiagnostic.decodedX,
            successfulDiagnostic.decodedY,
            successfulDiagnostic.decodedZ,
            true,
            successfulDiagnostic.verticalAdjustmentFirst,
            true,
            successfulDiagnostic.verticalAdjustmentSecond);
    return requested && active && objectSuccesses != 0 &&
        standardFallbacks == 0 &&
        successfulDiagnostic.error == RuntimeCoordinateCodecError::None &&
        successfulDiagnostic.stage ==
            RuntimeCoordinateCodecStage::RingDecoded &&
        finalized.Accepted() &&
        successfulDiagnostic.presentedZ == finalized.z;
}

inline bool IsAlgorithmCoordinateObjectProbeSuccessful(
    bool requested,
    bool active,
    std::uint64_t objectSuccesses,
    std::uint64_t standardFallbacks,
    const RuntimeCoordinateCodecDiagnostic& successfulDiagnostic) noexcept {
    return kAlgorithmCoordinateVisualAcceptanceCompleted &&
        IsAlgorithmCoordinateObjectSampleValid(
            requested,
            active,
            objectSuccesses,
            standardFallbacks,
            successfulDiagnostic);
}

}  // namespace lengjing::game::native
