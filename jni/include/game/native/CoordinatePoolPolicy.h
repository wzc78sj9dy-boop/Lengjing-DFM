#pragma once

#include <cstddef>
#include <cstdint>

namespace lengjing::game::native {

inline constexpr std::size_t kCoordinatePoolRingSearchesPerFrame = 4;
inline constexpr std::uint64_t kCoordinatePoolRingRetryFrames = 8;
inline constexpr std::uint64_t kCoordinatePoolCodeValidationRetryFrames = 8;
inline constexpr std::uint64_t kCoordinatePoolCodeValidationIdleFrame =
    UINT64_MAX;
inline constexpr std::uint64_t kCoordinatePoolPointerPayloadMask =
    UINT64_C(0x0000FFFFFFFFFFFF);
inline constexpr std::uint64_t kCoordinatePoolMinimumRemoteAddress =
    UINT64_C(0x10000000);
inline constexpr std::uint64_t kCoordinatePoolMaximumRemoteAddress =
    UINT64_C(0x10000000000);

constexpr bool CoordinatePoolEnvironmentFlagEnabled(
    const char* value) noexcept {
    return value != nullptr && value[0] == '1';
}

constexpr bool IsCoordinatePoolReadRangeValid(
    std::uint64_t address,
    std::size_t size) noexcept {
    return size != 0 && address >= kCoordinatePoolMinimumRemoteAddress &&
        address < kCoordinatePoolMaximumRemoteAddress &&
        size <= kCoordinatePoolMaximumRemoteAddress - address;
}

struct CoordinatePoolRootSnapshot {
    std::uint64_t bridge = 0;
    std::uint64_t context = 0;
    std::uint64_t entry = 0;
};

constexpr bool CoordinatePoolRootSnapshotsMatch(
    const CoordinatePoolRootSnapshot& left,
    const CoordinatePoolRootSnapshot& right) noexcept {
    return left.bridge == right.bridge && left.context == right.context &&
        left.entry == right.entry;
}

constexpr bool CoordinatePoolCodeIdentityChanged(
    const CoordinatePoolRootSnapshot& previous,
    const CoordinatePoolRootSnapshot& current) noexcept {
    return previous.entry != current.entry;
}

constexpr bool CoordinatePoolContextIdentityChanged(
    const CoordinatePoolRootSnapshot& previous,
    const CoordinatePoolRootSnapshot& current) noexcept {
    return previous.bridge != current.bridge ||
        previous.context != current.context;
}

class CoordinatePoolRootStabilityWindow final {
public:
    bool Observe(const CoordinatePoolRootSnapshot& snapshot) noexcept {
        const bool stable = ready_ &&
            CoordinatePoolRootSnapshotsMatch(previous_, snapshot);
        previous_ = snapshot;
        ready_ = true;
        return stable;
    }

    void Reset() noexcept {
        previous_ = {};
        ready_ = false;
    }

private:
    CoordinatePoolRootSnapshot previous_{};
    bool ready_ = false;
};

constexpr std::uint64_t NormalizeCoordinatePoolPointer(
    std::uint64_t value) noexcept {
    return value & kCoordinatePoolPointerPayloadMask;
}

inline std::uint64_t CoordinatePoolCodeFingerprint(
    const std::uint8_t* bytes,
    std::size_t size) noexcept {
    constexpr std::uint64_t kOffsetBasis = UINT64_C(14695981039346656037);
    constexpr std::uint64_t kPrime = UINT64_C(1099511628211);
    std::uint64_t fingerprint = kOffsetBasis;
    if (bytes == nullptr) return size == 0 ? fingerprint : 0;
    for (std::size_t index = 0; index < size; ++index) {
        fingerprint ^= bytes[index];
        fingerprint *= kPrime;
    }
    return fingerprint;
}

constexpr bool ShouldValidateCoordinatePoolCode(
    std::uint64_t frame,
    std::uint64_t nextValidationFrame,
    bool requested) noexcept {
    if (requested) return true;
    if (nextValidationFrame == kCoordinatePoolCodeValidationIdleFrame) {
        return false;
    }
    return frame >= nextValidationFrame ||
        nextValidationFrame - frame >
            kCoordinatePoolCodeValidationRetryFrames;
}

constexpr std::uint64_t NextCoordinatePoolCodeValidationFrame(
    std::uint64_t frame,
    bool validationSucceeded) noexcept {
    if (validationSucceeded) {
        return kCoordinatePoolCodeValidationIdleFrame;
    }
    return frame > UINT64_MAX - kCoordinatePoolCodeValidationRetryFrames
        ? UINT64_MAX
        : frame + kCoordinatePoolCodeValidationRetryFrames;
}

constexpr bool ShouldClearCoordinatePoolRingsAfterPointerRefresh(
    bool refreshSucceeded,
    std::uint64_t previousPointer,
    std::uint64_t refreshedPointer) noexcept {
    return refreshSucceeded && previousPointer != 0 &&
        previousPointer != refreshedPointer;
}

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
