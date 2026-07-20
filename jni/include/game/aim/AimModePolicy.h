#pragma once

#include "game/ProjectileTrackingFeature.h"

namespace lengjing::game::aim {

struct AimModeActivation {
    bool selfAim = false;
    bool tracking = false;

    constexpr bool Any() const noexcept {
        return selfAim || tracking;
    }
};

struct AimOutputAvailability {
    bool selfAim = false;
    bool tracking = false;

    constexpr bool Any() const noexcept {
        return selfAim || tracking;
    }
};

constexpr bool IsAimOutputRequested(
    bool selfAim,
    bool tracking) noexcept {
    return selfAim || IsProjectileTrackingRequested(tracking);
}

constexpr AimModeActivation ResolveAimModeActivation(
    bool outputEnabled,
    bool weaponAllowed,
    bool selfAim,
    bool tracking) noexcept {
    const bool available = outputEnabled && weaponAllowed;
    return AimModeActivation{
        available && selfAim,
        available && IsProjectileTrackingRequested(tracking),
    };
}

constexpr AimOutputAvailability ResolveAimOutputAvailability(
    const AimModeActivation& activation,
    bool selfAimTargetAvailable,
    bool trackingTargetAvailable) noexcept {
    return AimOutputAvailability{
        activation.selfAim && selfAimTargetAvailable,
        activation.tracking && trackingTargetAvailable,
    };
}

}  // namespace lengjing::game::aim
