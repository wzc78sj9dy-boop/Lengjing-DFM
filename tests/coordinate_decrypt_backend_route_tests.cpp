#include "test_support.h"

#include "game/native/CoordinateDecryptBackendRoute.h"

void RunCoordinateDecryptBackendRouteTests() {
    using lengjing::game::native::CoordinateExecutionMode;
    using lengjing::game::native::ResolveCoordinateDecryptBackendRoute;
    using lengjing::ui::CoordinateDecryptSelection;

    const auto none = ResolveCoordinateDecryptBackendRoute(
        CoordinateDecryptSelection::None);
    REQUIRE(!none.coordinateReplay);
    REQUIRE(!none.coordinateDecrypt2);
    REQUIRE(none.ExecutionModeValue() == 0);

    const auto decrypt1 = ResolveCoordinateDecryptBackendRoute(
        CoordinateDecryptSelection::Decrypt1);
    REQUIRE(decrypt1.coordinateReplay);
    REQUIRE(!decrypt1.coordinateDecrypt2);
    REQUIRE(decrypt1.ExecutionModeValue() == 0);

    const auto decrypt2 = ResolveCoordinateDecryptBackendRoute(
        CoordinateDecryptSelection::Decrypt2);
    REQUIRE(!decrypt2.coordinateReplay);
    REQUIRE(decrypt2.coordinateDecrypt2);
    REQUIRE(decrypt2.ExecutionModeValue() == 0);

    const struct {
        CoordinateDecryptSelection selection;
        CoordinateExecutionMode mode;
    } executionRoutes[] = {
        {CoordinateDecryptSelection::Decrypt3,
         CoordinateExecutionMode::Emulate},
        {CoordinateDecryptSelection::Decrypt4,
         CoordinateExecutionMode::Interpret},
        {CoordinateDecryptSelection::Decrypt5,
         CoordinateExecutionMode::Predecode},
        {CoordinateDecryptSelection::Decrypt6,
         CoordinateExecutionMode::Jit},
    };
    for (const auto& expected : executionRoutes) {
        const auto route = ResolveCoordinateDecryptBackendRoute(
            expected.selection);
        REQUIRE(!route.coordinateReplay);
        REQUIRE(!route.coordinateDecrypt2);
        REQUIRE(route.executionMode == expected.mode);
    }
}
