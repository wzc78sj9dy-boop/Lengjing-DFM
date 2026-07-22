#pragma once

#include "game/native/CharacterPositionResolver.h"

#include <cstdint>

namespace lengjing::game::native {

inline constexpr float kDecodedCharacterVerticalOffset = 90.0f;

enum class CharacterPositionSource : std::uint8_t {
    None,
    Standard,
    Decoded,
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

constexpr bool ShouldAlignBoneFrameToCharacterPosition(
    CharacterPositionSource source) noexcept {
    return source == CharacterPositionSource::Decoded ||
        source == CharacterPositionSource::AlgorithmObject ||
        source == CharacterPositionSource::AlgorithmTable;
}

}  // namespace lengjing::game::native
