#include "test_support.h"

#include "auth/CardInputPolicy.h"

#include <sstream>

void RunCardInputPolicyTests() {
    using namespace lengjing::auth::input;

    {
        std::istringstream input("  CARD_FOR_TEST  \n");
        std::ostringstream output;
        const CardInputResult result =
            ReadCardKeyFromStream(input, output, false);
        REQUIRE(result.status == CardInputStatus::Accepted);
        REQUIRE(result.value == "CARD_FOR_TEST");
        REQUIRE(output.str().empty());
    }

    {
        std::istringstream input("CARD_FOR_TEST\n");
        std::ostringstream output;
        const CardInputResult result =
            ReadCardKeyFromStream(input, output, true);
        REQUIRE(result.status == CardInputStatus::Accepted);
        REQUIRE(output.str() == "请输入卡密: ");
    }

    {
        std::istringstream input("   \n");
        std::ostringstream output;
        const CardInputResult result =
            ReadCardKeyFromStream(input, output, true);
        REQUIRE(result.status == CardInputStatus::Empty);
        REQUIRE(output.str() == "请输入卡密: ");
    }

    {
        std::istringstream input("BAD\tCARD\n");
        std::ostringstream output;
        const CardInputResult result =
            ReadCardKeyFromStream(input, output, false);
        REQUIRE(result.status == CardInputStatus::Invalid);
        REQUIRE(output.str().empty());
    }

    {
        std::istringstream input;
        std::ostringstream output;
        const CardInputResult result =
            ReadCardKeyFromStream(input, output, false);
        REQUIRE(result.status == CardInputStatus::EndOfInput);
        REQUIRE(output.str().empty());
    }
}
