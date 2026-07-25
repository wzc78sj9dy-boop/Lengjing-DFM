#include "test_support.h"

#include "game/native/CoordinateDecryptBackendRoute.h"

void RunCoordinateDecryptBackendRouteTests() {
    using lengjing::game::native::CoordinateExecutionMode;
    using lengjing::game::native::ResolveCoordinateDecryptBackendRoute;
    using lengjing::ui::CoordinateDecryptSelection;

    const auto none = ResolveCoordinateDecryptBackendRoute(
        CoordinateDecryptSelection::None, true);
    REQUIRE(!none.coordinateReplay);
    REQUIRE(!none.coordinateDecrypt2);
    REQUIRE(none.ExecutionModeValue() == 0);

    const auto decrypt1 = ResolveCoordinateDecryptBackendRoute(
        CoordinateDecryptSelection::Decrypt1, true);
    REQUIRE(decrypt1.coordinateReplay);
    REQUIRE(!decrypt1.coordinateDecrypt2);
    REQUIRE(decrypt1.ExecutionModeValue() == 0);

    for (const CoordinateDecryptSelection selection : {
             CoordinateDecryptSelection::Decrypt2,
             CoordinateDecryptSelection::Decrypt3,
             CoordinateDecryptSelection::Decrypt4,
             CoordinateDecryptSelection::Decrypt5,
             CoordinateDecryptSelection::Decrypt6,
         }) {
        const auto route = ResolveCoordinateDecryptBackendRoute(
            selection, true);
        REQUIRE(!route.coordinateReplay);
        REQUIRE(route.coordinateDecrypt2);
        REQUIRE(route.ExecutionModeValue() == 0);
    }

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
            expected.selection, false);
        REQUIRE(!route.coordinateReplay);
        REQUIRE(!route.coordinateDecrypt2);
        REQUIRE(route.executionMode == expected.mode);
    }
}
