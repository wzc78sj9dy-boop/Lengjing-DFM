#include "test_support.h"

#include "app/CoordinateProbePolicy.h"
#include "app/RuntimeExitPolicy.h"

void RunRuntimeExitPolicyTests() {
    using namespace lengjing;

    REQUIRE(app::ResolveRuntimeExitCode(
                true,
                game::RuntimePhase::Faulted,
                game::RuntimeFailureKind::CloudLayoutRejected) ==
            auth::kCloudLayoutStartupFailureExitCode);
    REQUIRE(app::ResolveRuntimeExitCode(
                false,
                game::RuntimePhase::Faulted,
                game::RuntimeFailureKind::CloudLayoutRejected) == 0);
    REQUIRE(app::ResolveRuntimeExitCode(
                true,
                game::RuntimePhase::Faulted,
                game::RuntimeFailureKind::None) == 0);
    REQUIRE(app::ResolveRuntimeExitCode(
                true,
                game::RuntimePhase::Running,
                game::RuntimeFailureKind::CloudLayoutRejected) == 0);

    const auto legacy = app::ResolveCoordinateProbeSettings(false);
    REQUIRE(app::IsAlgorithmCoordinateProbeEnabled("1"));
    REQUIRE(!app::IsAlgorithmCoordinateProbeEnabled("0"));
    REQUIRE(!app::IsAlgorithmCoordinateProbeEnabled("true"));
    REQUIRE(!app::IsAlgorithmCoordinateProbeEnabled("10"));
    REQUIRE(legacy.coordinateDecrypt);
    REQUIRE(!legacy.algorithmDecrypt);
    const auto algorithmOnly = app::ResolveCoordinateProbeSettings(true);
    REQUIRE(!algorithmOnly.coordinateDecrypt);
    REQUIRE(algorithmOnly.algorithmDecrypt);

    REQUIRE(app::ResolveCoordinateProbeExitCode(
                true, 17, 9, true, true, 0) == 17);
    REQUIRE(app::ResolveCoordinateProbeExitCode(
                true, 0, 9, true, true, 1) == 0);
    REQUIRE(app::ResolveCoordinateProbeExitCode(
                true, 0, 9, true, true, 0) == 11);
    REQUIRE(app::ResolveCoordinateProbeExitCode(
                false, 0, 1, false, false, 0) == 0);
    REQUIRE(app::ResolveCoordinateProbeExitCode(
                false, 0, 0, true, false, 0) == 13);
    REQUIRE(app::ResolveCoordinateProbeExitCode(
                false, 0, 0, false, true, 0) == 12);
    REQUIRE(app::ResolveCoordinateProbeExitCode(
                false, 0, 0, false, false, 1) == 11);
}
