#pragma once

#include "game/native/CharacterPositionResolver.h"

namespace lengjing::game::native {

inline constexpr float kDecodedCharacterVerticalOffset = 90.0f;

constexpr PositionReadMode ResolvePositionReadMode(
    bool coordinateDecrypt) noexcept {
    return coordinateDecrypt
        ? PositionReadMode::Direct
        : PositionReadMode::Standard;
}

constexpr float ResolveDecodedCharacterZ(float decodedZ) noexcept {
    return decodedZ - kDecodedCharacterVerticalOffset;
}

}  // namespace lengjing::game::native
