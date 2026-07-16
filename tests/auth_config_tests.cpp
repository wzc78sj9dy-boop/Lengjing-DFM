#include "test_support.h"

#include "auth/AuthConfig.h"

#include <string_view>

void RunAuthConfigTests() {
    using namespace lengjing::auth::config;

    REQUIRE(IsComplete());
    REQUIRE(kLoginCode.size() == 16);
    REQUIRE(kNoticeCode.size() == 16);
    REQUIRE(kVersionCode.size() == 16);
    REQUIRE(kHeartbeatCode.size() == 16);
    REQUIRE(kAppKey.size() == 32);
    REQUIRE(kRsaPublicKey.find("-----BEGIN PUBLIC KEY-----") == 0);
    REQUIRE(
        kRsaPublicKey.find("-----END PUBLIC KEY-----") !=
        std::string_view::npos);
    REQUIRE(kRsaPublicKey.find("__") == std::string_view::npos);
    REQUIRE(kHeartbeatSeconds == 60);
    REQUIRE(kMaximumHeartbeatFailures == 5);
}
