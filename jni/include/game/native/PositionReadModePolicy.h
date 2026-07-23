#pragma once

#include "game/native/AlgorithmCoordinateDiagnostics.h"
#include "game/native/CharacterPositionResolver.h"

#include <cmath>
#include <cstdint>

namespace lengjing::game::native {

inline constexpr float kDecodedCharacterVerticalOffset = 90.0f;

enum class CharacterPositionSource : std::uint8_t {
    None,
    Standard,
    Decoded,
    HardwareBreakpoint,
    AlgorithmObject,
    AlgorithmTable,
};

constexpr PositionReadMode ResolvePositionReadMode(
    bool coordinateDecrypt) noexcept {
    return coordinateDecrypt
        ? PositionReadMode::Direct
        : PositionReadMode::Standard;
}

constexpr float ResolveDecodedCharacterZ(float decodedZ) noexcept {
    return decodedZ - kDecodedCharacterVerticalOffset;
}

enum class AlgorithmCoordinateFinalizeError : std::uint8_t {
    None,
    RawInvalid,
    VerticalAdjustmentReadFailed,
    VerticalAdjustmentInvalid,
    VerticalAdjustmentUnstable,
    OutputInvalid,
};

struct AlgorithmCoordinateFinalizeResult {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    AlgorithmCoordinateFinalizeError error =
        AlgorithmCoordinateFinalizeError::RawInvalid;

    bool Accepted() const noexcept {
        return error == AlgorithmCoordinateFinalizeError::None;
    }
};

inline AlgorithmCoordinateFinalizeResult
FinalizeAlgorithmCharacterCoordinate(
    float rawX,
    float rawY,
    float rawZ,
    bool firstVerticalAdjustmentRead,
    float firstVerticalAdjustment,
    bool secondVerticalAdjustmentRead,
    float secondVerticalAdjustment) noexcept {
    AlgorithmCoordinateFinalizeResult result{};
    if (!IsValidAlgorithmCoordinateValue(rawX, rawY, rawZ)) {
        return result;
    }
    if (!firstVerticalAdjustmentRead || !secondVerticalAdjustmentRead) {
        result.error =
            AlgorithmCoordinateFinalizeError::VerticalAdjustmentReadFailed;
        return result;
    }
    if (!std::isfinite(firstVerticalAdjustment) ||
        !std::isfinite(secondVerticalAdjustment) ||
        firstVerticalAdjustment <= 0.0f ||
        secondVerticalAdjustment <= 0.0f) {
        result.error =
            AlgorithmCoordinateFinalizeError::VerticalAdjustmentInvalid;
        return result;
    }
    if (firstVerticalAdjustment != secondVerticalAdjustment) {
        result.error =
            AlgorithmCoordinateFinalizeError::VerticalAdjustmentUnstable;
        return result;
    }

    result.x = rawX;
    result.y = rawY;
    result.z = rawZ - secondVerticalAdjustment;
    if (!IsValidAlgorithmCoordinateValue(result.x, result.y, result.z)) {
        result = {};
        result.error = AlgorithmCoordinateFinalizeError::OutputInvalid;
        return result;
    }
    result.error = AlgorithmCoordinateFinalizeError::None;
    return result;
}

constexpr bool ShouldAlignBoneFrameToCharacterPosition(
    CharacterPositionSource source) noexcept {
    return source == CharacterPositionSource::Decoded ||
        source == CharacterPositionSource::HardwareBreakpoint ||
        source == CharacterPositionSource::AlgorithmObject ||
        source == CharacterPositionSource::AlgorithmTable;
}

}  // namespace lengjing::game::native
