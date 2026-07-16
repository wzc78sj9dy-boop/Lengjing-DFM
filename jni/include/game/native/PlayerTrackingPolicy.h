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

constexpr std::uint64_t ResolvePlayerIdentity(
    std::uintptr_t actor,
    std::uintptr_t playerState) noexcept {
    return static_cast<std::uint64_t>(
        playerState != 0 ? playerState : actor);
}

}  // namespace lengjing::game::native
