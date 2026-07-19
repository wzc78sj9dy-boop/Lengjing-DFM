#include "game/native/FrameProjection.h"
#include "test_support.h"

#include <cmath>

void RunFrameProjectionTests() {
    using namespace lengjing::game::native;

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
}
