#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

namespace lengjing::game {

enum class CoordinateDecryptError : std::uint16_t {
    None = 0,

    InvalidConfiguration = 1001,
    EntryResolveFailed = 1002,
    EntryMappingMissing = 1003,
    EntryCodeReadFailed = 1004,
    CodeAnalysisFailed = 1005,

    MemoryTransportUnavailable = 2001,
    RootReadFailed = 2002,
    ContextDeviceOpenFailed = 2003,
    ContextDeviceProtocolMismatch = 2004,
    ContextThreadMissing = 2005,
    ContextPermissionDenied = 2006,
    ContextReadFailed = 2007,
    ContextDataInvalid = 2008,
    ComponentReadFailed = 2009,
    PtraceOracleResolveFailed = 2010,
    PtraceExecutionFailed = 2011,
    PtracePacgaOperandsUnavailable = 2012,

    EngineSetupFailed = 5001,
    ParameterExecutionFailed = 5003,
    ParameterReadFailed = 5004,
    PoolPointerReadFailed = 5005,
    RingSearchFailed = 5006,
    PacgaUnavailable = 5007,
    ReplayExecutionFailed = 5011,
    PositionReadFailed = 5012,

    OutputNotFinite = 6002,
    OutputZero = 6003,
    OutputUnstable = 6004,

    UnhandledException = 9001,
};

constexpr std::uint16_t CoordinateDecryptErrorCode(
    CoordinateDecryptError error) noexcept {
    return static_cast<std::uint16_t>(error);
}

inline std::string FormatCoordinateDecryptDiagnostic(
    CoordinateDecryptError error,
    int systemError) {
    std::array<char, 40> message{};
    std::snprintf(
        message.data(),
        message.size(),
        "COORD CD-%04u SYS=%d",
        static_cast<unsigned int>(CoordinateDecryptErrorCode(error)),
        systemError);
    return message.data();
}

}  // namespace lengjing::game
