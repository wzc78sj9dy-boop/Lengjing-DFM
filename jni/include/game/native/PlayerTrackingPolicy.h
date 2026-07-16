#pragma once

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

constexpr bool IsPlayerTrackable(
    const PlayerTrackingData& tracking,
    const PlayerDirectionData&) noexcept {
    return tracking.coordinateAvailable &&
        tracking.healthAvailable &&
        tracking.aliveOrDowned;
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
    return explicitlyDowned ||
        (health <= 0.0f &&
         downedHealth > 0.0f &&
         maximumDownedHealth > 0.0f);
}

}  // namespace lengjing::game::native
