#include "test_support.h"

#include "auth/AuthConfig.h"

void RunAuthConfigTests() {
    using namespace lengjing::auth;

    REQUIRE(!kDefaultT3AuthConfig.IsLoginConfigured());
    REQUIRE(kHeartbeatIntervalSeconds == 60);
    REQUIRE(kMaximumHeartbeatFailures == 5);
    REQUIRE(!kDefaultT3AuthConfig.cloudVariable.IsConfigured());
    REQUIRE(!kDefaultT3AuthConfig.cloudVariable.HasAnyValue());
    REQUIRE(
        !kDefaultT3AuthConfig.coordinateDecrypt2Variable.IsConfigured());
    REQUIRE(
        !kDefaultT3AuthConfig.coordinateDecrypt2Variable.HasAnyValue());
    REQUIRE(!kDefaultT3AuthConfig.cloudIdentity.IsConfigured());
    REQUIRE(kDefaultT3AuthConfig.cloudIdentity.packageName ==
            "com.tencent.tmgp.dfm");
    REQUIRE(kDefaultT3AuthConfig.cloudIdentity.moduleName == "libUE4.so");
    REQUIRE(!kDefaultCoordinateRemotePlanConfig.IsConfigured());
    REQUIRE(kDefaultCoordinateRemotePlanConfig.url.empty());
    REQUIRE(kDefaultCoordinateRemotePlanConfig.deviceId.empty());
    REQUIRE(kDefaultCoordinateRemotePlanConfig.seedPath.empty());

    constexpr CloudVariableConfig complete{
        "CALL_CODE", "VALUE_ID", "VALUE_NAME"};
    constexpr CloudVariableConfig partial{"CALL_CODE", {}, {}};
    REQUIRE(complete.IsConfigured());
    REQUIRE(partial.HasAnyValue());
    REQUIRE(!partial.IsConfigured());

    constexpr CloudIdentityConfig identity{
        "com.example.runtime", "libUE4.so",
        "0123456789abcdef0123456789abcdef01234567"};
    REQUIRE(identity.IsConfigured());

    constexpr CoordinateRemotePlanConfig remotePlan{
        "http://HOST:8080/decrypt/parse", {}, "/data/adb/.device_seed"};
    REQUIRE(remotePlan.IsConfigured());
}
