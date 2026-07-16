#pragma once

#include <chrono>

namespace lengjing::game {

using FrameClock = std::chrono::steady_clock;

inline constexpr auto kWaitingFrameGrace = std::chrono::milliseconds(250);

constexpr bool ShouldPublishWaitingFrame(
    bool hasReadyFrame,
    FrameClock::duration timeSinceReadyFrame) noexcept {
    return !hasReadyFrame || timeSinceReadyFrame >= kWaitingFrameGrace;
}

}  // namespace lengjing::game
