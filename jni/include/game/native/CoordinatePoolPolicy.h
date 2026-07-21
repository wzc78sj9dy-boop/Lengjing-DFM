#pragma once

#include <cstddef>
#include <cstdint>

namespace lengjing::game::native {

inline constexpr std::size_t kCoordinatePoolRingSearchesPerFrame = 4;
inline constexpr std::uint64_t kCoordinatePoolRingRetryFrames = 8;

constexpr std::uint64_t CoordinatePoolRingRefreshPhase(
    std::uintptr_t component,
    std::uint32_t interval) noexcept {
    if (interval == 0) return 0;
    const std::uint64_t mixed =
        (static_cast<std::uint64_t>(component) >> 4U) ^
        (static_cast<std::uint64_t>(component) >> 17U);
    return mixed % interval;
}

constexpr bool ShouldRefreshCoordinatePoolRing(
    std::uintptr_t component,
    std::uint64_t stamp,
    std::uint64_t frame,
    std::uint32_t interval) noexcept {
    return interval != 0 && frame >= stamp &&
        frame - stamp >= interval &&
        frame % interval ==
            CoordinatePoolRingRefreshPhase(component, interval);
}

constexpr bool ShouldRetryCoordinatePoolRing(
    std::uint64_t stamp,
    std::uint64_t frame) noexcept {
    return frame < stamp || frame - stamp >= kCoordinatePoolRingRetryFrames;
}

class CoordinatePoolRingSearchBudget final {
public:
    bool TryConsume(std::uint64_t frame) noexcept {
        if (!ready_ || frame_ != frame) {
            frame_ = frame;
            used_ = 0;
            ready_ = true;
        }
        if (used_ >= kCoordinatePoolRingSearchesPerFrame) return false;
        ++used_;
        return true;
    }

    void Reset() noexcept {
        frame_ = 0;
        used_ = 0;
        ready_ = false;
    }

private:
    std::uint64_t frame_ = 0;
    std::size_t used_ = 0;
    bool ready_ = false;
};

}  // namespace lengjing::game::native
