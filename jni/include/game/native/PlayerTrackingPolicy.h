#pragma once

#include <cmath>
#include <cstdint>

namespace lengjing::game::native {

inline constexpr int kUnknownPlayerTeam = -1;

struct PlayerTrackingData {
    bool coordinateAvailable = false;
    bool healthAvailable = false;
    bool aliveOrDowned = false;
};

struct PlayerDirectionData {
    bool rotationAvailable = false;
};

constexpr PlayerTrackingData MakePlayerTrackingData(
    bool coordinateAvailable,
    bool healthAvailable,
    float health,
    bool downed) noexcept {
    return PlayerTrackingData{
        coordinateAvailable,
        healthAvailable,
        healthAvailable && (health > 0.0f || downed),
    };
}

constexpr bool IsPlayerTrackable(
    const PlayerTrackingData& tracking,
    const PlayerDirectionData&) noexcept {
    return tracking.coordinateAvailable &&
        tracking.healthAvailable &&
        tracking.aliveOrDowned;
}

constexpr bool IsPlayerVisualEligible(
    bool trackable,
    bool downed,
    bool showDowned) noexcept {
    return trackable && (!downed || showDowned);
}

constexpr bool HasUsablePlayerState(
    bool playerStateAvailable,
    bool botClass) noexcept {
    return playerStateAvailable || botClass;
}

constexpr int ResolvePlayerTeam(
    bool teamRead,
    int team,
    bool botClass) noexcept {
    if (teamRead) {
        return team;
    }
    return botClass ? 0 : kUnknownPlayerTeam;
}

constexpr bool HasComparablePlayerTeams(
    int localTeam,
    int targetTeam) noexcept {
    return localTeam > 0 && targetTeam > 0;
}

constexpr bool IsSamePlayerTeam(
    int localTeam,
    int targetTeam) noexcept {
    return HasComparablePlayerTeams(localTeam, targetTeam) &&
        localTeam == targetTeam;
}

constexpr bool IsEnemyEligible(
    int localTeam,
    int targetTeam,
    bool botClass) noexcept {
    return HasComparablePlayerTeams(localTeam, targetTeam)
        ? localTeam != targetTeam
        : botClass;
}

inline bool IsWithinPlayerDrawRange(
    float horizontalDistanceMeters,
    int drawDistanceMeters) noexcept {
    return std::isfinite(horizontalDistanceMeters) &&
        horizontalDistanceMeters >= 0.0f &&
        (drawDistanceMeters <= 0 ||
         horizontalDistanceMeters <= static_cast<float>(drawDistanceMeters));
}

inline bool IsWithinOffscreenWarningRange(
    float horizontalDistanceMeters,
    int drawDistanceMeters,
    float warningDistanceMeters) noexcept {
    return IsWithinPlayerDrawRange(
               horizontalDistanceMeters, drawDistanceMeters) &&
        std::isfinite(warningDistanceMeters) &&
        warningDistanceMeters > 0.0f &&
        horizontalDistanceMeters <= warningDistanceMeters;
}

constexpr std::uint64_t ResolvePlayerIdentity(
    std::uintptr_t actor,
    std::uintptr_t playerState) noexcept {
    return static_cast<std::uint64_t>(
        playerState != 0 ? playerState : actor);
}

constexpr bool ResolveDownedState(
    bool explicitlyDowned,
    float health,
    float downedHealth,
    float maximumDownedHealth) noexcept {
    return health <= 0.0f &&
        (explicitlyDowned ||
         (downedHealth > 0.0f && maximumDownedHealth > 0.0f));
}

}  // namespace lengjing::game::native
