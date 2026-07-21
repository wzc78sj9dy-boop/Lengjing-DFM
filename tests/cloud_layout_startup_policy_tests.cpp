#include "test_support.h"

#include "auth/CloudLayoutStartupPolicy.h"

void RunCloudLayoutStartupPolicyTests() {
    using namespace lengjing::auth;

    REQUIRE(kCloudLayoutStartupFailureExitCode == 3);
    REQUIRE(ResolveCloudLayoutStartupAction(
                false, false, false, false, false) ==
            CloudLayoutStartupAction::UseBuiltInLayout);
    REQUIRE(ResolveCloudLayoutStartupAction(
                true, false, false, false, false) ==
            CloudLayoutStartupAction::StopStartup);
    REQUIRE(ResolveCloudLayoutStartupAction(
                true, true, false, false, false) ==
            CloudLayoutStartupAction::FetchCloudLayout);
    REQUIRE(ResolveCloudLayoutStartupAction(
                true, true, true, false, false) ==
            CloudLayoutStartupAction::StopStartup);
    REQUIRE(ResolveCloudLayoutStartupAction(
                true, true, true, true, false) ==
            CloudLayoutStartupAction::StopStartup);
    REQUIRE(ResolveCloudLayoutStartupAction(
                true, true, true, true, true) ==
            CloudLayoutStartupAction::UseCloudLayout);
}
