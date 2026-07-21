#include "test_support.h"

#include "game/aim/AimPrediction.h"

#include <cmath>
#include <limits>

namespace {

bool NearlyEqual(float left, float right, float tolerance = 0.0001f) {
    return std::fabs(left - right) <= tolerance;
}

void RequirePoint(
    const lengjing::game::aim::Vec3& actual,
    const lengjing::game::aim::Vec3& expected) {
    REQUIRE(NearlyEqual(actual.x, expected.x));
    REQUIRE(NearlyEqual(actual.y, expected.y));
    REQUIRE(NearlyEqual(actual.z, expected.z));
}

}  // namespace

void RunAimPredictionTests() {
    using lengjing::game::aim::PredictAimPoint;
    using lengjing::game::aim::PredictInterceptPoint;
    using lengjing::game::aim::TargetSnapshot;
    using lengjing::game::aim::Vec3;

    TargetSnapshot target{};
    target.world = Vec3{1000.0f, 2000.0f, 3000.0f};
    target.velocity = Vec3{100.0f, 40.0f, 20.0f};
    target.worldDistanceMeters = 100.0f;
    target.projectileSpeedCmPerSecond = 10000.0f;
    target.tuning.prediction = 1.0f;

    const Vec3 slowProjectile = PredictInterceptPoint(target);
    RequirePoint(slowProjectile, Vec3{1100.0f, 2040.0f, 3020.054f});

    target.projectileSpeedCmPerSecond = 20000.0f;
    const Vec3 fastProjectile = PredictInterceptPoint(target);
    RequirePoint(fastProjectile, Vec3{1050.0f, 2020.0f, 3010.0134f});
    REQUIRE(slowProjectile.x > fastProjectile.x);

    target.projectileSpeedCmPerSecond = 10000.0f;
    target.tuning.prediction = 0.5f;
    const Vec3 halfPrediction = PredictInterceptPoint(target);
    RequirePoint(halfPrediction, Vec3{1050.0f, 2020.0f, 3010.054f});

    target.tuning.prediction = 2.0f;
    const Vec3 doublePrediction = PredictInterceptPoint(target);
    RequirePoint(doublePrediction, Vec3{1200.0f, 2080.0f, 3040.054f});

    target.projectileSpeedCmPerSecond = 0.0f;
    const Vec3 fallbackSpeed = PredictInterceptPoint(target);
    REQUIRE(fallbackSpeed.x > target.world.x);
    REQUIRE(fallbackSpeed.y > target.world.y);
    target.projectileSpeedCmPerSecond =
        std::numeric_limits<float>::infinity();
    RequirePoint(PredictInterceptPoint(target), fallbackSpeed);

    target.worldDistanceMeters = 20.0f;
    target.projectileSpeedCmPerSecond = 10000.0f;
    target.tuning.prediction = 1.0f;
    const Vec3 closeTarget = PredictInterceptPoint(target);
    REQUIRE(NearlyEqual(closeTarget.x, 1036.3636f, 0.001f));
    REQUIRE(NearlyEqual(closeTarget.y, 2014.5454f, 0.001f));

    target.worldDistanceMeters = 100.0f;
    target.projectileSpeedCmPerSecond = 10000.0f;
    target.tuning.prediction = 1.0f;
    target.tuning.recoil = 1.0f;
    target.tuning.hipSpeed = 30.0f;
    target.firing = true;
    target.zooming = false;
    const Vec3 interceptWithRecoilConfigured = PredictInterceptPoint(target);

    TargetSnapshot noRecoil = target;
    noRecoil.tuning.recoil = 0.0f;
    RequirePoint(
        interceptWithRecoilConfigured,
        PredictInterceptPoint(noRecoil));

    const Vec3 aimed = PredictAimPoint(target);
    REQUIRE(NearlyEqual(aimed.x, interceptWithRecoilConfigured.x));
    REQUIRE(NearlyEqual(aimed.y, interceptWithRecoilConfigured.y));
    REQUIRE(aimed.z < interceptWithRecoilConfigured.z);
    RequirePoint(PredictAimPoint(noRecoil), interceptWithRecoilConfigured);
}
