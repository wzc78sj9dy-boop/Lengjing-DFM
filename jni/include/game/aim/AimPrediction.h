#pragma once

#include "game/aim/AimController.h"

#include <algorithm>
#include <cmath>

namespace lengjing::game::aim {

inline Vec3 PredictInterceptPoint(const TargetSnapshot& target) noexcept {
    Vec3 predicted = target.world;
    const bool moving = target.velocity.x != 0.0f ||
        target.velocity.y != 0.0f || target.velocity.z != 0.0f;
    if (target.tuning.prediction > 0.0f && moving &&
        std::isfinite(target.worldDistanceMeters) &&
        std::isfinite(target.velocity.x) &&
        std::isfinite(target.velocity.y) &&
        std::isfinite(target.velocity.z) &&
        std::isfinite(target.tuning.prediction)) {
        const float speed =
            std::isfinite(target.projectileSpeedCmPerSecond) &&
            target.projectileSpeedCmPerSecond > 5000.0f &&
            target.projectileSpeedCmPerSecond < 500000.0f
                ? target.projectileSpeedCmPerSecond
                : 88000.0f;
        const float distance = target.worldDistanceMeters;
        const float distanceScale = distance >= 40.0f ? 0.01f : 0.0055f;
        const float flightTime =
            distance / (speed * distanceScale) * target.tuning.prediction;
        const float bulletFlightTime = distance / speed;
        if (std::isfinite(flightTime) && flightTime >= 0.0f) {
            predicted.x += target.velocity.x * flightTime;
            predicted.y += target.velocity.y * flightTime;
            predicted.z += target.velocity.z * flightTime +
                540.0f * bulletFlightTime * bulletFlightTime;
        }
    }
    return predicted;
}

inline Vec3 PredictAimPoint(const TargetSnapshot& target) noexcept {
    Vec3 predicted = PredictInterceptPoint(target);
    if (target.firing && std::isfinite(target.tuning.recoil) &&
        target.tuning.recoil > 0.0f) {
        const float distance = target.worldDistanceMeters;
        const float speed = target.zooming
            ? target.tuning.adsSpeed
            : target.tuning.hipSpeed;
        const float activeSpeed = std::isfinite(speed) ? speed : 30.0f;
        const float decay =
            1.0f / (1.0f + std::pow(distance / 45.0f, 1.2f));
        predicted.z -= static_cast<int>(distance) * activeSpeed *
            target.tuning.recoil * decay;
    }
    return predicted;
}

}  // namespace lengjing::game::aim
