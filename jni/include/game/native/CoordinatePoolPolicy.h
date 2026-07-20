#pragma once

#include <cstdint>

namespace lengjing::game::native {

inline constexpr int kCoordinatePoolStableReadAttempts = 4;
inline constexpr std::uint64_t kCoordinatePoolPointerPayloadMask =
    UINT64_C(0x0000FFFFFFFFFFFF);

constexpr std::uint64_t NormalizeCoordinatePoolPointer(
    std::uint64_t pointer) noexcept {
    return pointer & kCoordinatePoolPointerPayloadMask;
}

constexpr bool ShouldRefreshCoordinatePoolState(
    std::uint64_t frame,
    std::uint64_t lastRefreshFrame,
    std::uint32_t refreshFrames) noexcept {
    return lastRefreshFrame == 0 || frame < lastRefreshFrame ||
        frame - lastRefreshFrame >= refreshFrames;
}

constexpr bool ShouldRetryCoordinatePoolRing(
    bool usedCachedRing,
    bool stablePositionRead) noexcept {
    return usedCachedRing && !stablePositionRead;
}

}  // namespace lengjing::game::native
