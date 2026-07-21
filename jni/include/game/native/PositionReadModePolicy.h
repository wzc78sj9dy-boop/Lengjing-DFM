#pragma once

#include "game/native/CharacterPositionResolver.h"

namespace lengjing::game::native {

inline constexpr float kDecodedCharacterVerticalOffset = 90.0f;
inline constexpr float kDecodedBotExtraVerticalOffset = 90.0f;

constexpr PositionReadMode ResolvePositionReadMode(
    bool coordinateDecrypt) noexcept {
    return coordinateDecrypt
        ? PositionReadMode::Direct
        : PositionReadMode::Standard;
}

constexpr float ResolveDecodedCharacterZ(
    float decodedZ,
    bool botCharacter = false) noexcept {
    return decodedZ - kDecodedCharacterVerticalOffset -
        (botCharacter ? kDecodedBotExtraVerticalOffset : 0.0f);
}

}  // namespace lengjing::game::native
