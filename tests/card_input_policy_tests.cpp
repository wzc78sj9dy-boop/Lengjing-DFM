#include "test_support.h"

#include "auth/CardInputPolicy.h"

#include <sstream>

namespace {

struct EchoProbe {
    int disableCalls = 0;
    int restoreCalls = 0;
    bool disableResult = true;
    bool restoreResult = true;
};

bool DisableEcho(void* opaque) noexcept {
    auto& probe = *static_cast<EchoProbe*>(opaque);
    ++probe.disableCalls;
    return probe.disableResult;
}

bool RestoreEcho(void* opaque) noexcept {
    auto& probe = *static_cast<EchoProbe*>(opaque);
    ++probe.restoreCalls;
    return probe.restoreResult;
}

lengjing::auth::input::TerminalEchoControl EchoControl(
    EchoProbe& probe) {
    return {&probe, &DisableEcho, &RestoreEcho};
}

}  // namespace

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
        std::istringstream input("y\n");
        std::ostringstream output;
        const CardInputResult result = ReadCardKeyFromStream(
            input, output, false, {}, "SAVED_CARD_FOR_TEST");
        REQUIRE(result.status == CardInputStatus::Accepted);
        REQUIRE(result.value == "SAVED_CARD_FOR_TEST");
        REQUIRE(output.str().empty());
    }

    {
        std::istringstream input("\xEF\xBB\xBF" "y\n");
        std::ostringstream output;
        const CardInputResult result = ReadCardKeyFromStream(
            input, output, false, {}, "SAVED_CARD_FOR_TEST");
        REQUIRE(result.status == CardInputStatus::Accepted);
        REQUIRE(result.value == "SAVED_CARD_FOR_TEST");
    }

    {
        std::istringstream input("Y\n");
        std::ostringstream output;
        const CardInputResult result = ReadCardKeyFromStream(
            input, output, true, {}, "SAVED_CARD_FOR_TEST");
        REQUIRE(result.status == CardInputStatus::Accepted);
        REQUIRE(result.value == "SAVED_CARD_FOR_TEST");
        REQUIRE(
            output.str() ==
            "请输入卡密，输入 y 复用上次卡密: ");
    }

    {
        std::istringstream input("y\n");
        std::ostringstream output;
        const CardInputResult result =
            ReadCardKeyFromStream(input, output, false);
        REQUIRE(result.status == CardInputStatus::ReuseUnavailable);
        REQUIRE(result.value.empty());
    }

    {
        std::istringstream input("NEW_CARD_FOR_TEST\n");
        std::ostringstream output;
        const CardInputResult result = ReadCardKeyFromStream(
            input, output, true, {}, "SAVED_CARD_FOR_TEST");
        REQUIRE(result.status == CardInputStatus::Accepted);
        REQUIRE(result.value == "NEW_CARD_FOR_TEST");
        REQUIRE(
            output.str() ==
            "请输入卡密，输入 y 复用上次卡密: ");
    }

    {
        std::istringstream input;
        std::ostringstream output;
        const CardInputResult result =
            ReadCardKeyFromStream(input, output, false);
        REQUIRE(result.status == CardInputStatus::EndOfInput);
        REQUIRE(output.str().empty());
    }

    {
        EchoProbe probe;
        std::istringstream input("CARD_FOR_TEST\n");
        std::ostringstream output;
        const CardInputResult result = ReadCardKeyFromStream(
            input, output, true, EchoControl(probe));
        REQUIRE(result.status == CardInputStatus::Accepted);
        REQUIRE(result.value == "CARD_FOR_TEST");
        REQUIRE(output.str() == "请输入卡密: \n");
        REQUIRE(probe.disableCalls == 1);
        REQUIRE(probe.restoreCalls == 1);
    }

    {
        EchoProbe probe;
        std::istringstream input("CARD_FOR_TEST\n");
        std::ostringstream output;
        const CardInputResult result = ReadCardKeyFromStream(
            input, output, false, EchoControl(probe));
        REQUIRE(result.status == CardInputStatus::Accepted);
        REQUIRE(output.str().empty());
        REQUIRE(probe.disableCalls == 0);
        REQUIRE(probe.restoreCalls == 0);
    }

    {
        EchoProbe probe;
        probe.disableResult = false;
        std::istringstream input("CARD_FOR_TEST\n");
        std::ostringstream output;
        const CardInputResult result = ReadCardKeyFromStream(
            input, output, true, EchoControl(probe));
        REQUIRE(result.status == CardInputStatus::TerminalError);
        REQUIRE(result.value.empty());
        REQUIRE(output.str() == "请输入卡密: ");
        REQUIRE(probe.disableCalls == 1);
        REQUIRE(probe.restoreCalls == 0);
        std::string unread;
        REQUIRE(static_cast<bool>(std::getline(input, unread)));
        REQUIRE(unread == "CARD_FOR_TEST");
    }

    {
        EchoProbe probe;
        probe.restoreResult = false;
        std::istringstream input("CARD_FOR_TEST\n");
        std::ostringstream output;
        const CardInputResult result = ReadCardKeyFromStream(
            input, output, true, EchoControl(probe));
        REQUIRE(result.status == CardInputStatus::TerminalError);
        REQUIRE(result.value.empty());
        REQUIRE(output.str() == "请输入卡密: \n");
        REQUIRE(probe.disableCalls == 1);
        REQUIRE(probe.restoreCalls == 2);
    }

    {
        EchoProbe probe;
        std::istringstream input;
        input.exceptions(std::ios::failbit | std::ios::badbit);
        std::ostringstream output;
        bool threw = false;
        try {
            (void)ReadCardKeyFromStream(
                input, output, true, EchoControl(probe));
        } catch (const std::ios_base::failure&) {
            threw = true;
        }
        REQUIRE(threw);
        REQUIRE(probe.disableCalls == 1);
        REQUIRE(probe.restoreCalls == 1);
    }
}
