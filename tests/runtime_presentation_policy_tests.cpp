#include "test_support.h"

#include "app/RuntimePresentationPolicy.h"
#include "game/CoordinateDecryptDiagnostics.h"

#include <array>
#include <string>
#include <string_view>

void RunRuntimePresentationPolicyTests() {
    using namespace lengjing::app;

    constexpr std::array<std::uint64_t, 2> successes{0, 1};
    for (const bool requested : {false, true}) {
        for (const bool entryReady : {false, true}) {
            for (const bool contextReady : {false, true}) {
                for (const std::uint64_t successCount : successes) {
                    const CoordinateDecryptPresentation presentation =
                        ResolveCoordinateDecryptPresentation(
                            requested,
                            entryReady,
                            contextReady,
                            successCount);
                    const bool expectedSuccess = requested && entryReady &&
                        contextReady && successCount != 0;
                    REQUIRE(
                        (presentation ==
                         CoordinateDecryptPresentation::Succeeded) ==
                        expectedSuccess);

                    const std::string_view text =
                        CoordinateDecryptPresentationText(presentation);
                    REQUIRE(text == "解密成功" || text == "解密失败");
                    REQUIRE((text == "解密成功") == expectedSuccess);
                    REQUIRE(
                        ShouldNotifyCoordinateDecryptSuccess(
                            presentation, false) == expectedSuccess);
                    REQUIRE(!ShouldNotifyCoordinateDecryptSuccess(
                        presentation, true));
                }
            }
        }
    }

    REQUIRE(std::string_view(VerificationFailureText()) ==
            "[验证] 验证失败");
    REQUIRE(std::string_view(RuntimeFaultText()) == "运行模块发生错误");
    REQUIRE(std::string_view(UpdateRequiredText()) ==
            "请到网盘更新最新版本");
    REQUIRE(std::string_view(RuntimeDataUnavailableText()) ==
            "运行数据暂不可用");
    REQUIRE(std::string_view(RuntimeDataRestoredText()) ==
            "运行数据已恢复");

    const std::string diagnostic =
        lengjing::game::FormatCoordinateDecryptDiagnostic(
            lengjing::game::CoordinateDecryptError::
                ContextDeviceProtocolMismatch,
            -71);
    REQUIRE(diagnostic == "COORD CD-2004 SYS=-71");
    REQUIRE(diagnostic.find('\n') == std::string::npos);
}
