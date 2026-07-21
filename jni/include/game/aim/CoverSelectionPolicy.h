#pragma once

#include "game/VisibilityState.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace lengjing::game::aim {

enum class CoverSelectionMode : std::uint8_t {
    AllVisiblePoints = 0,
    PreferredPointOnly = 1,
};

struct CoverPointSelection {
    static constexpr std::size_t kUnavailableIndex =
        std::numeric_limits<std::size_t>::max();

    bool selected = false;
    std::size_t pointIndex = kUnavailableIndex;
    VisibilityState visibility = VisibilityState::Unavailable;
};

constexpr CoverSelectionMode NormalizeCoverSelectionMode(int mode) noexcept {
    return mode == static_cast<int>(CoverSelectionMode::PreferredPointOnly)
        ? CoverSelectionMode::PreferredPointOnly
        : CoverSelectionMode::AllVisiblePoints;
}

constexpr std::size_t PreferredCoverPoint(int aimPointMode) noexcept {
    return aimPointMode == 0 ? 0U : 1U;
}

template <std::size_t N>
CoverPointSelection SelectCoverPoint(
    int coverMode,
    int aimPointMode,
    const std::array<bool, N>& valid,
    const std::array<VisibilityState, N>& visibility) noexcept {
    const auto select = [&](std::size_t index) noexcept {
        if (index >= N || !valid[index] ||
            visibility[index] != VisibilityState::Visible) {
            return CoverPointSelection{};
        }
        return CoverPointSelection{true, index, visibility[index]};
    };

    const std::size_t preferred = PreferredCoverPoint(aimPointMode);
    CoverPointSelection selected = select(preferred);
    if (selected.selected ||
        NormalizeCoverSelectionMode(coverMode) ==
            CoverSelectionMode::PreferredPointOnly) {
        return selected;
    }

    const std::array<std::size_t, 3> primaryOrder = preferred == 0U
        ? std::array<std::size_t, 3>{0U, 1U, 2U}
        : std::array<std::size_t, 3>{1U, 0U, 2U};
    for (const std::size_t index : primaryOrder) {
        if (index == preferred) continue;
        selected = select(index);
        if (selected.selected) return selected;
    }

    for (std::size_t index = 0; index < N; ++index) {
        if (index == 0U || index == 1U || index == 2U) continue;
        selected = select(index);
        if (selected.selected) return selected;
    }
    return {};
}

}  // namespace lengjing::game::aim
