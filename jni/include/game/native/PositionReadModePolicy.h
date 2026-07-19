#pragma once

#include "game/native/CharacterPositionResolver.h"

namespace lengjing::game::native {

constexpr PositionReadMode ResolvePositionReadMode(
    bool coordinateDecrypt) noexcept {
    return coordinateDecrypt
        ? PositionReadMode::Direct
        : PositionReadMode::Standard;
}

}  // namespace lengjing::game::native
