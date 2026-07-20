#pragma once

#ifndef LENGJING_ENABLE_PROJECTILE_TRACKING
#define LENGJING_ENABLE_PROJECTILE_TRACKING 0
#endif

#if LENGJING_ENABLE_PROJECTILE_TRACKING != 0 && \
    LENGJING_ENABLE_PROJECTILE_TRACKING != 1
#error "LENGJING_ENABLE_PROJECTILE_TRACKING must be 0 or 1"
#endif

namespace lengjing::game {

inline constexpr bool kProjectileTrackingCompiled =
    LENGJING_ENABLE_PROJECTILE_TRACKING != 0;

constexpr bool ResolveProjectileTrackingRequest(
    bool compiled,
    bool requested) noexcept {
    return compiled && requested;
}

constexpr bool IsProjectileTrackingRequested(bool requested) noexcept {
    return ResolveProjectileTrackingRequest(
        kProjectileTrackingCompiled, requested);
}

}  // namespace lengjing::game
