#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace lengjing::game::native {

using DecodedPositionClock = std::chrono::steady_clock;

struct DecodedPositionCacheIdentity {
    std::uintptr_t world = 0;
    std::uintptr_t actor = 0;
    std::uintptr_t coordinateIdentity = 0;
};

enum class DecodedPositionCacheIdentityState : std::uint8_t {
    Unknown = 0,
    Match,
    Mismatch,
};

inline constexpr auto kDecodedPositionRetention = std::chrono::seconds(3);
inline constexpr auto kDecodedPositionRecoveryAge =
    std::chrono::milliseconds(2500);
inline constexpr auto kCoordinateFailureRecoveryDelay =
    std::chrono::seconds(2);
inline constexpr auto kAgedDecodedFailureRecoveryDelay =
    std::chrono::milliseconds(250);

constexpr bool ShouldKeepDecodedPositionSource(
    bool coordinateDecryptRequested,
    bool directMode) noexcept {
    return coordinateDecryptRequested && directMode;
}

inline constexpr bool kAlgorithmCoordinateEnabled = false;

constexpr bool ShouldReadAlgorithmCoordinate(
    bool,
    bool algorithmDecryptRequested) noexcept {
    return kAlgorithmCoordinateEnabled && algorithmDecryptRequested;
}

constexpr bool ShouldBlockStandardCoordinateFallback(
    bool coordinateDecryptRequested,
    bool algorithmDecryptRequested,
    bool algorithmCoordinateAvailable) noexcept {
    return ShouldReadAlgorithmCoordinate(
               coordinateDecryptRequested,
               algorithmDecryptRequested) &&
        !algorithmCoordinateAvailable;
}

constexpr bool IsCoordinateFrameHealthy(
    std::size_t attempts,
    std::size_t successes,
    bool = false) noexcept {
    return attempts != 0 && successes != 0;
}

constexpr DecodedPositionCacheIdentityState
ClassifyDecodedPositionCacheIdentity(
    const DecodedPositionCacheIdentity& cached,
    const DecodedPositionCacheIdentity& current) noexcept {
    if (current.world == 0 || current.actor == 0) {
        return DecodedPositionCacheIdentityState::Unknown;
    }
    if (cached.world != current.world || cached.actor != current.actor) {
        return DecodedPositionCacheIdentityState::Mismatch;
    }
    if (current.coordinateIdentity == 0) {
        return DecodedPositionCacheIdentityState::Unknown;
    }
    return cached.coordinateIdentity == current.coordinateIdentity
        ? DecodedPositionCacheIdentityState::Match
        : DecodedPositionCacheIdentityState::Mismatch;
}

constexpr bool IsDecodedPositionCacheOwnerMatch(
    const DecodedPositionCacheIdentity& cached,
    const DecodedPositionCacheIdentity& current) noexcept {
    return current.world != 0 && current.actor != 0 &&
        cached.world == current.world && cached.actor == current.actor;
}

constexpr bool ShouldDiscardDecodedPositionCache(
    DecodedPositionCacheIdentityState identityState) noexcept {
    return identityState == DecodedPositionCacheIdentityState::Mismatch;
}

inline bool CanUseDecodedPositionHistory(
    DecodedPositionCacheIdentityState identityState,
    DecodedPositionClock::time_point capturedAt,
    DecodedPositionClock::time_point now) noexcept {
    return identityState == DecodedPositionCacheIdentityState::Match &&
        now >= capturedAt && now - capturedAt <= kDecodedPositionRetention;
}

inline bool CanRetainDecodedPosition(
    bool antiFlicker,
    const DecodedPositionCacheIdentity& cachedIdentity,
    const DecodedPositionCacheIdentity& currentIdentity,
    DecodedPositionClock::time_point updatedAt,
    DecodedPositionClock::time_point now) noexcept {
    return antiFlicker &&
        ClassifyDecodedPositionCacheIdentity(
            cachedIdentity, currentIdentity) ==
            DecodedPositionCacheIdentityState::Match &&
        now >= updatedAt && now - updatedAt <= kDecodedPositionRetention;
}

inline bool ShouldEscalateDecodedPositionFailure(
    DecodedPositionClock::time_point updatedAt,
    DecodedPositionClock::time_point now) noexcept {
    return now >= updatedAt &&
        now - updatedAt >= kDecodedPositionRecoveryAge;
}

constexpr DecodedPositionClock::duration CoordinateFailureRecoveryDelay(
    bool agedDecodedFailure) noexcept {
    return agedDecodedFailure
        ? kAgedDecodedFailureRecoveryDelay
        : kCoordinateFailureRecoveryDelay;
}

inline bool HasCoordinateFailureRecoveryElapsed(
    DecodedPositionClock::time_point failureSince,
    DecodedPositionClock::time_point now,
    bool agedDecodedFailure) noexcept {
    return failureSince.time_since_epoch().count() != 0 &&
        now >= failureSince &&
        now - failureSince >=
            CoordinateFailureRecoveryDelay(agedDecodedFailure);
}

}  // namespace lengjing::game::native
