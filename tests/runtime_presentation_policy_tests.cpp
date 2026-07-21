#include "test_support.h"

#include "app/RuntimePresentationPolicy.h"
#include "game/CoordinateDecryptDiagnostics.h"
#include "game/RuntimeDiagnostics.h"

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

    lengjing::game::CoordinateReadDiagnostic read{};
    read.stage = lengjing::game::CoordinateReadStage::CodePage;
    read.primaryPath = lengjing::game::CoordinateReadPath::ProcessVm;
    read.lastPath = lengjing::game::CoordinateReadPath::ProcMem;
    read.failure = lengjing::game::CoordinateReadFailure::ProcMemOpenFailed;
    read.attemptedPaths =
        lengjing::game::CoordinateReadPathMask(
            lengjing::game::CoordinateReadPath::ProcessVm) |
        lengjing::game::CoordinateReadPathMask(
            lengjing::game::CoordinateReadPath::ProcMem);
    read.attemptCount = 4;
    read.address = 0x12345000U;
    read.size = 4096;
    read.primarySystemError = -1;
    read.lastSystemError = -13;
    read.systemError = -13;
    const std::string detailedDiagnostic =
        lengjing::game::FormatCoordinateDecryptDiagnostic(
            lengjing::game::CoordinateDecryptError::
                EntryCodeProcMemOpenFailed,
            read.systemError,
            read);
    REQUIRE(detailedDiagnostic ==
        "COORD CD-1012 SYS=-13 READ=PMEM_OPEN STAGE=CODE_PAGE "
        "PRI=PVM P_SYS=-1 P_DONE=0 LAST=PMEM L_SYS=-13 L_DONE=0 "
        "TRY=0x3 CALLS=4 AT=0x12345000 N=4096");
    REQUIRE(detailedDiagnostic.find('\n') == std::string::npos);
    struct ReadErrorMapping {
        lengjing::game::CoordinateReadFailure failure;
        lengjing::game::CoordinateDecryptError error;
    };
    constexpr std::array<ReadErrorMapping, 10> readErrorMappings{{
        {lengjing::game::CoordinateReadFailure::None,
         lengjing::game::CoordinateDecryptError::None},
        {lengjing::game::CoordinateReadFailure::InvalidRange,
         lengjing::game::CoordinateDecryptError::EntryCodeReadInvalidRange},
        {lengjing::game::CoordinateReadFailure::TransportUnavailable,
         lengjing::game::CoordinateDecryptError::EntryCodeReadUnavailable},
        {lengjing::game::CoordinateReadFailure::PermissionDenied,
         lengjing::game::CoordinateDecryptError::
             EntryCodeReadPermissionDenied},
        {lengjing::game::CoordinateReadFailure::AddressFault,
         lengjing::game::CoordinateDecryptError::EntryCodeReadAddressFault},
        {lengjing::game::CoordinateReadFailure::ShortRead,
         lengjing::game::CoordinateDecryptError::EntryCodeReadShort},
        {lengjing::game::CoordinateReadFailure::ProcessVmReadFailed,
         lengjing::game::CoordinateDecryptError::
             EntryCodeProcessVmReadFailed},
        {lengjing::game::CoordinateReadFailure::ProcMemOpenFailed,
         lengjing::game::CoordinateDecryptError::
             EntryCodeProcMemOpenFailed},
        {lengjing::game::CoordinateReadFailure::ProcMemReadFailed,
         lengjing::game::CoordinateDecryptError::
             EntryCodeProcMemReadFailed},
        {lengjing::game::CoordinateReadFailure::MappingChanged,
         lengjing::game::CoordinateDecryptError::EntryMappingChanged},
    }};
    for (const ReadErrorMapping& mapping : readErrorMappings) {
        REQUIRE(lengjing::game::CoordinateReadError(mapping.failure) ==
                mapping.error);
    }
    lengjing::game::CoordinateReadDiagnostic changedRead = read;
    changedRead.address += 4096;
    REQUIRE(changedRead != read);

    const std::string runtimeDiagnostic =
        lengjing::game::FormatRuntimeDiagnostic(
            lengjing::game::RuntimeError::ModuleReadFailed,
            -5);
    REQUIRE(runtimeDiagnostic == "RUNTIME RT-1302 SYS=-5");
    REQUIRE(runtimeDiagnostic.find('\n') == std::string::npos);
}
