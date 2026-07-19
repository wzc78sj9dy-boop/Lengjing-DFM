#pragma once

#include "game/aim/AimController.h"

#include <algorithm>
#include <cmath>

namespace lengjing::game::aim {

inline Vec3 PredictInterceptPoint(const TargetSnapshot& target) noexcept {
    Vec3 predicted = target.world;
    const float speed = target.projectileSpeedCmPerSecond;
    if (std::isfinite(target.velocity.x) &&
        std::isfinite(target.velocity.y) &&
        std::isfinite(target.velocity.z) &&
        std::isfinite(speed) && speed >= 5000.0f && speed <= 500000.0f &&
        std::isfinite(target.tuning.prediction)) {
        const float seconds = target.worldDistanceMeters * 100.0f / speed *
            std::clamp(target.tuning.prediction, 0.0f, 2.0f);
        if (std::isfinite(seconds) && seconds >= 0.0f) {
            predicted.x += target.velocity.x * seconds;
            predicted.y += target.velocity.y * seconds;
            predicted.z += target.velocity.z * seconds +
                230.0f * seconds * seconds;
        }
    }
    return predicted;
}

inline Vec3 PredictAimPoint(const TargetSnapshot& target) noexcept {
    Vec3 predicted = PredictInterceptPoint(target);
    if (target.firing && std::isfinite(target.tuning.recoil) &&
        target.tuning.recoil > 0.0f) {
        const float distance =
            std::clamp(target.worldDistanceMeters, 0.0f, 500.0f);
        const float speed = target.zooming
            ? target.tuning.adsSpeed
            : target.tuning.hipSpeed;
        const float activeSpeed = std::clamp(
            std::isfinite(speed) ? speed : 30.0f, 1.0f, 100.0f);
        const float decay =
            1.0f / (1.0f + std::pow(distance / 45.0f, 1.2f));
        predicted.z -= distance * activeSpeed *
            std::clamp(target.tuning.recoil, 0.0f, 2.0f) * decay;
    }
    return predicted;
}

}  // namespace lengjing::game::aim
