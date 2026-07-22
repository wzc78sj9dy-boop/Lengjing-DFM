#include "test_support.h"

#include "app/RuntimePresentationPolicy.h"
#include "game/CoordinateDecryptDiagnostics.h"
#include "game/RuntimeDiagnostics.h"
#include "game/native/AlgorithmCoordinateDiagnostics.h"
#include "game/native/RuntimeCoordinateCodec.h"

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
    REQUIRE(detailedDiagnostic == "COORD CD-1012 SYS=-13");
    REQUIRE(detailedDiagnostic.find('\n') == std::string::npos);

    lengjing::game::CoordinatePoolPointerDiagnostic poolPointer{};
    poolPointer.error = lengjing::game::CoordinateDecryptError::
        PoolPointerAddressInvalid;
    poolPointer.stateFlags = 0xFU;
    poolPointer.offset = 152;
    poolPointer.computedContext = 0x12340000U;
    poolPointer.address = 0x42U;
    poolPointer.systemError = -34;
    const std::string poolDiagnostic =
        lengjing::game::FormatCoordinateDecryptDiagnostic(
            lengjing::game::CoordinateDecryptError::RingSearchFailed,
            0,
            {},
            poolPointer);
    REQUIRE(poolDiagnostic ==
        "COORD CD-5006 SYS=0 DETAIL=CD-5009 D_SYS=-34");
    REQUIRE(lengjing::game::CoordinateDecryptErrorCode(
                lengjing::game::CoordinateDecryptError::
                    PoolPointerReadFailed) == 5005);
    REQUIRE(lengjing::game::CoordinateDecryptErrorCode(
                lengjing::game::CoordinateDecryptError::
                    PoolPointerOffsetMissing) == 5008);
    REQUIRE(lengjing::game::CoordinateDecryptErrorCode(
                lengjing::game::CoordinateDecryptError::
                    PoolPointerValueInvalid) == 5010);
    REQUIRE(lengjing::game::CoordinateDecryptErrorCode(
                lengjing::game::CoordinateDecryptError::
                    RingPreparationFailed) == 5013);
    REQUIRE(lengjing::game::CoordinateDecryptErrorCode(
                lengjing::game::CoordinateDecryptError::RingValueInvalid) ==
            5016);
    REQUIRE(lengjing::game::CoordinateDecryptErrorCode(
                lengjing::game::CoordinateDecryptError::EntryMappingFragmented) ==
            1015);
    REQUIRE(lengjing::game::CoordinateDecryptErrorCode(
                lengjing::game::CoordinateDecryptError::EntryCodePageReadFailed) ==
            1016);

    lengjing::game::CoordinateEntryDiagnostic entry{};
    entry.entry = 0x71234000U;
    entry.mappingStart = 0x71200000U;
    entry.mappingEnd = 0x71400000U;
    entry.failedMethod = 0x71345000U;
    entry.mappingFragments = 7;
    const std::string entryDiagnostic =
        lengjing::game::FormatCoordinateDecryptDiagnostic(
            lengjing::game::CoordinateDecryptError::EntryMappingFragmented,
            -34,
            {},
            {},
            entry);
    REQUIRE(entryDiagnostic == "COORD CD-1015 SYS=-34");
    REQUIRE(entry != lengjing::game::CoordinateEntryDiagnostic{});

    for (const std::string* sanitized :
         {&detailedDiagnostic, &poolDiagnostic, &entryDiagnostic}) {
        REQUIRE(sanitized->find("0x") == std::string::npos);
        REQUIRE(sanitized->find("READ=") == std::string::npos);
        REQUIRE(sanitized->find("ENTRY=") == std::string::npos);
        REQUIRE(sanitized->find("MAP=") == std::string::npos);
        REQUIRE(sanitized->find("P_CTX=") == std::string::npos);
    }

    lengjing::game::native::AlgorithmCoordinateDiagnostic algorithm{};
    algorithm.error = lengjing::game::native::
        AlgorithmCoordinateReadError::CoordinateInvalid;
    algorithm.table = 0x12345678U;
    algorithm.actor = 0x87654321U;
    algorithm.x = 1.0f;
    REQUIRE(lengjing::game::native::FormatAlgorithmCoordinateDiagnostic(
                algorithm) == "ALGO AC-0014");

    lengjing::game::native::RuntimeCoordinateCodecDiagnostic codec{};
    codec.error =
        lengjing::game::native::RuntimeCoordinateCodecError::OutputInvalid;
    codec.hook = 0x12345678U;
    codec.encodedFieldKey = UINT64_C(0x1122334455667788);
    codec.decodedX = 1.0f;
    REQUIRE(lengjing::game::native::FormatRuntimeCoordinateCodecDiagnostic(
                codec) == "ALGO RC-0028");

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
