#pragma once

#include <cstdint>

namespace lengjing::game {

enum class VisibilityState : std::uint8_t {
    Unavailable,
    Visible,
    Occluded,
};

}  // namespace lengjing::game
