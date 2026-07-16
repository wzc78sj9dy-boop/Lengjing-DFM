#include "test_support.h"

#include "auth/CardInputPolicy.h"

#include <sstream>
#include <string>

void RunCardInputPolicyTests() {
    using lengjing::auth::input::ReadCardKeyFromStream;

    {
        std::istringstream input("  PIPE_CARD  \n");
        std::ostringstream output;
        REQUIRE(
            ReadCardKeyFromStream(input, output, false, "CACHED_CARD") ==
            "PIPE_CARD");
        REQUIRE(output.str().empty());
    }

    {
        std::istringstream input("MANUAL_CARD\n");
        std::ostringstream output;
        REQUIRE(
            ReadCardKeyFromStream(input, output, true, {}) ==
            "MANUAL_CARD");
        REQUIRE(output.str().find("请输入卡密") != std::string::npos);
    }

    {
        std::istringstream input("y\n");
        std::ostringstream output;
        REQUIRE(
            ReadCardKeyFromStream(input, output, true, "CACHED_CARD") ==
            "CACHED_CARD");
        REQUIRE(
            output.str().find("输入y使用上次登录卡密") !=
            std::string::npos);
    }
}
