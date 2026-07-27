#pragma once

#include "game/native/CharacterPositionResolver.h"

#include <cstdint>

namespace lengjing::game::native {

enum class CharacterPositionSource : std::uint8_t {
    None,
    Standard,
    Resolved,
    HardwareBreakpoint,
};

constexpr bool ShouldRequireResolvedActorRecords(
    PositionReadMode positionMode,
    bool trajectoryTracking) noexcept {
    return trajectoryTracking ||
        positionMode == PositionReadMode::ResolvedRecord;
}

constexpr bool ShouldAlignBoneFrameToCharacterPosition(
    CharacterPositionSource source) noexcept {
    return source == CharacterPositionSource::Resolved ||
        source == CharacterPositionSource::HardwareBreakpoint;
}

}  // namespace lengjing::game::native
