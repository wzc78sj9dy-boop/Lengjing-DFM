#include "game/native/FrameProjection.h"
#include "test_support.h"

#include <cmath>

void RunFrameProjectionTests() {
    using namespace lengjing::game::native;

    REQUIRE(IsProjectionViewCacheCompatible(
        true, 0x1000, 0x2000, 0x1000, 0x2000));
    REQUIRE(!IsProjectionViewCacheCompatible(
        false, 0x1000, 0x2000, 0x1000, 0x2000));
    REQUIRE(!IsProjectionViewCacheCompatible(
        true, 0x1000, 0x2000, 0x3000, 0x2000));
    REQUIRE(!IsProjectionViewCacheCompatible(
        true, 0x1000, 0x2000, 0x1000, 0x4000));

    ProjectionFovStabilityState fovState{};
    float resolvedFov = 0.0f;
    REQUIRE(ResolveStableProjectionFov(
        0x1000, 0x2000, 1, true, 90.0f, true, 90.0f,
        fovState, resolvedFov));
    REQUIRE(std::fabs(resolvedFov - 90.0f) < 0.001f);

    REQUIRE(ResolveStableProjectionFov(
        0x1000, 0x2000, 2, true, 60.0f, true, 60.0f,
        fovState, resolvedFov));
    REQUIRE(std::fabs(resolvedFov - 90.0f) < 0.001f);
    REQUIRE(ResolveStableProjectionFov(
        0x1000, 0x2000, 2, true, 60.0f, true, 60.0f,
        fovState, resolvedFov));
    REQUIRE(std::fabs(resolvedFov - 90.0f) < 0.001f);
    REQUIRE(ResolveStableProjectionFov(
        0x1000, 0x2000, 3, true, 60.0f, true, 60.0f,
        fovState, resolvedFov));
    REQUIRE(std::fabs(resolvedFov - 60.0f) < 0.001f);

    REQUIRE(ResolveStableProjectionFov(
        0x1000, 0x2000, 4, true, 60.0f, true, 75.0f,
        fovState, resolvedFov));
    REQUIRE(std::fabs(resolvedFov - 60.0f) < 0.001f);
    REQUIRE(ResolveStableProjectionFov(
        0x3000, 0x4000, 4, true, 100.0f, true, 100.0f,
        fovState, resolvedFov));
    REQUIRE(std::fabs(resolvedFov - 100.0f) < 0.001f);
    REQUIRE(ResolveStableProjectionFov(
        0x3000, 0x4000, 5, false, 0.0f, false, 0.0f,
        fovState, resolvedFov));
    REQUIRE(std::fabs(resolvedFov - 100.0f) < 0.001f);

    ProjectionView view{};
    view.fieldOfView = 90.0f;
    const ProjectionPoint target{100.0f, 0.0f, 0.0f};
    const ScreenProjection centered =
        ProjectWorldPoint(target, view, 2400, 1080);
    REQUIRE(centered.valid);
    REQUIRE(std::fabs(centered.x - 1200.0f) < 0.01f);
    REQUIRE(std::fabs(centered.y - 540.0f) < 0.01f);

    ProjectionView latest = view;
    latest.location.y = 10.0f;
    const ScreenProjection refreshed =
        ProjectWorldPoint(target, latest, 2400, 1080);
    REQUIRE(refreshed.valid);
    REQUIRE(refreshed.x < centered.x);

    latest.rotation.yaw = 180.0f;
    REQUIRE(!ProjectWorldPoint(target, latest, 2400, 1080).valid);

    ProjectionView rolled = view;
    rolled.rotation.roll = 90.0f;
    const ScreenProjection rollRotated = ProjectWorldPoint(
        {100.0f, 0.0f, -10.0f}, rolled, 2400, 1080);
    REQUIRE(rollRotated.valid);
    REQUIRE(rollRotated.x > 1200.0f);
    REQUIRE(std::fabs(rollRotated.y - 540.0f) < 0.01f);
}
