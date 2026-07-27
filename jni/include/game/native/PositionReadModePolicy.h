#pragma once

#include "game/native/CharacterPositionResolver.h"

#include <cstdint>

namespace lengjing::game::native {

enum class CharacterPositionSource : std::uint8_t {
    None,
    Standard,
    HardwareBreakpoint,
};

constexpr bool ShouldAlignBoneFrameToCharacterPosition(
    CharacterPositionSource source) noexcept {
    return source == CharacterPositionSource::HardwareBreakpoint;
}

}  // namespace lengjing::game::native
