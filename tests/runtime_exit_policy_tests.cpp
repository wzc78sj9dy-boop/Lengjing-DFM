#include "test_support.h"

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
}
