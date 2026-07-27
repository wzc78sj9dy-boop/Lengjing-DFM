#include "test_support.h"

#include "auth/AuthConfig.h"

void RunAuthConfigTests() {
    using namespace lengjing::auth;

    REQUIRE(!kDefaultT3AuthConfig.IsLoginConfigured());
    REQUIRE(kHeartbeatIntervalSeconds == 60);
    REQUIRE(kMaximumHeartbeatFailures == 5);
    REQUIRE(!kDefaultT3AuthConfig.coordinateSuiteVariable.IsConfigured());
    REQUIRE(!kDefaultT3AuthConfig.coordinateSuiteVariable.HasAnyValue());
    REQUIRE(!kDefaultT3AuthConfig.IsCoordinateSuiteConfigured());
    REQUIRE(!kDefaultT3AuthConfig.targetPackage.empty());
    REQUIRE(!kDefaultT3AuthConfig.targetModule.empty());

    constexpr CloudVariableConfig complete{
        "CALL_CODE", "VALUE_ID", "VALUE_NAME"};
    constexpr CloudVariableConfig partial{"CALL_CODE", {}, {}};
    REQUIRE(complete.IsConfigured());
    REQUIRE(partial.HasAnyValue());
    REQUIRE(!partial.IsConfigured());

    constexpr T3AuthConfig configured{
        "LOGIN", "NOTICE", "VERSION", "HEARTBEAT", "APP", "RSA",
        complete, "com.example.target", "libExample.so"};
    REQUIRE(configured.IsLoginConfigured());
    REQUIRE(configured.IsCoordinateSuiteConfigured());

}
