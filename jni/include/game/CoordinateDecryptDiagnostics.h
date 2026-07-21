#pragma once

#include <array>
#include <cstddef>
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
    EntryCodeReadInvalidRange = 1006,
    EntryCodeReadUnavailable = 1007,
    EntryCodeReadPermissionDenied = 1008,
    EntryCodeReadAddressFault = 1009,
    EntryCodeReadShort = 1010,
    EntryCodeProcessVmReadFailed = 1011,
    EntryCodeProcMemOpenFailed = 1012,
    EntryCodeProcMemReadFailed = 1013,
    EntryMappingChanged = 1014,

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
    PoolPointerStateInvalid = 5002,
    ParameterExecutionFailed = 5003,
    ParameterReadFailed = 5004,
    PoolPointerReadFailed = 5005,
    RingSearchFailed = 5006,
    PacgaUnavailable = 5007,
    PoolPointerOffsetMissing = 5008,
    PoolPointerAddressInvalid = 5009,
    PoolPointerValueInvalid = 5010,
    ReplayExecutionFailed = 5011,
    PositionReadFailed = 5012,
    RingPreparationFailed = 5013,
    RingExecutionFailed = 5014,
    RingRegisterReadFailed = 5015,
    RingValueInvalid = 5016,

    OutputNotFinite = 6002,
    OutputZero = 6003,
    OutputUnstable = 6004,

    UnhandledException = 9001,
};

enum class CoordinateReadPath : std::uint8_t {
    None = 0,
    ProcessVm = 1,
    ProcMem = 2,
};

inline constexpr std::array<CoordinateReadPath, 2>
    kCoordinateReadPathOrder{
        CoordinateReadPath::ProcessVm,
        CoordinateReadPath::ProcMem,
    };

template <typename Attempt>
bool TryCoordinateReadPaths(Attempt&& attempt) {
    for (const CoordinateReadPath path : kCoordinateReadPathOrder) {
        if (attempt(path)) return true;
    }
    return false;
}

enum class CoordinateReadStage : std::uint8_t {
    None = 0,
    Root,
    Context,
    Entry,
    CodePage,
    DynamicPage,
    Parameter,
    PoolPointer,
    RingIndex,
    Position,
};

enum class CoordinateReadFailure : std::uint8_t {
    None = 0,
    InvalidRange,
    TransportUnavailable,
    PermissionDenied,
    AddressFault,
    ShortRead,
    ProcessVmReadFailed,
    ProcMemOpenFailed,
    ProcMemReadFailed,
    MappingChanged,
};

constexpr std::uint32_t CoordinateReadPathMask(
    CoordinateReadPath path) noexcept {
    const auto value = static_cast<std::uint8_t>(path);
    return value == 0 ? 0U : (1U << (value - 1U));
}

struct CoordinateReadDiagnostic {
    CoordinateReadStage stage = CoordinateReadStage::None;
    CoordinateReadPath primaryPath = CoordinateReadPath::None;
    CoordinateReadPath lastPath = CoordinateReadPath::None;
    CoordinateReadFailure failure = CoordinateReadFailure::None;
    std::uint32_t attemptedPaths = 0;
    std::uint32_t attemptCount = 0;
    std::uintptr_t address = 0;
    std::size_t size = 0;
    std::size_t primaryCompleted = 0;
    std::size_t lastCompleted = 0;
    int primarySystemError = 0;
    int lastSystemError = 0;
    int systemError = 0;

    constexpr bool HasFailure() const noexcept {
        return failure != CoordinateReadFailure::None;
    }
};

struct CoordinatePoolPointerDiagnostic {
    CoordinateDecryptError error = CoordinateDecryptError::None;
    std::uint32_t stateFlags = 0;
    std::int32_t offset = 0;
    std::uintptr_t computedContext = 0;
    std::uintptr_t address = 0;
    std::uint64_t rawValue = 0;
    std::uint64_t normalizedValue = 0;
    int systemError = 0;
    CoordinateReadDiagnostic read{};

    constexpr bool HasFailure() const noexcept {
        return error != CoordinateDecryptError::None;
    }
};

constexpr bool operator==(const CoordinateReadDiagnostic& left,
                          const CoordinateReadDiagnostic& right) noexcept {
    return left.stage == right.stage &&
        left.primaryPath == right.primaryPath &&
        left.lastPath == right.lastPath && left.failure == right.failure &&
        left.attemptedPaths == right.attemptedPaths &&
        left.attemptCount == right.attemptCount &&
        left.address == right.address && left.size == right.size &&
        left.primaryCompleted == right.primaryCompleted &&
        left.lastCompleted == right.lastCompleted &&
        left.primarySystemError == right.primarySystemError &&
        left.lastSystemError == right.lastSystemError &&
        left.systemError == right.systemError;
}

constexpr bool operator!=(const CoordinateReadDiagnostic& left,
                          const CoordinateReadDiagnostic& right) noexcept {
    return !(left == right);
}

constexpr bool operator==(
    const CoordinatePoolPointerDiagnostic& left,
    const CoordinatePoolPointerDiagnostic& right) noexcept {
    return left.error == right.error &&
        left.stateFlags == right.stateFlags && left.offset == right.offset &&
        left.computedContext == right.computedContext &&
        left.address == right.address && left.rawValue == right.rawValue &&
        left.normalizedValue == right.normalizedValue &&
        left.systemError == right.systemError && left.read == right.read;
}

constexpr bool operator!=(
    const CoordinatePoolPointerDiagnostic& left,
    const CoordinatePoolPointerDiagnostic& right) noexcept {
    return !(left == right);
}

constexpr std::uint16_t CoordinateDecryptErrorCode(
    CoordinateDecryptError error) noexcept {
    return static_cast<std::uint16_t>(error);
}

constexpr CoordinateDecryptError CoordinateReadError(
    CoordinateReadFailure failure) noexcept {
    switch (failure) {
        case CoordinateReadFailure::None:
            return CoordinateDecryptError::None;
        case CoordinateReadFailure::InvalidRange:
            return CoordinateDecryptError::EntryCodeReadInvalidRange;
        case CoordinateReadFailure::TransportUnavailable:
            return CoordinateDecryptError::EntryCodeReadUnavailable;
        case CoordinateReadFailure::PermissionDenied:
            return CoordinateDecryptError::EntryCodeReadPermissionDenied;
        case CoordinateReadFailure::AddressFault:
            return CoordinateDecryptError::EntryCodeReadAddressFault;
        case CoordinateReadFailure::ShortRead:
            return CoordinateDecryptError::EntryCodeReadShort;
        case CoordinateReadFailure::ProcessVmReadFailed:
            return CoordinateDecryptError::EntryCodeProcessVmReadFailed;
        case CoordinateReadFailure::ProcMemOpenFailed:
            return CoordinateDecryptError::EntryCodeProcMemOpenFailed;
        case CoordinateReadFailure::ProcMemReadFailed:
            return CoordinateDecryptError::EntryCodeProcMemReadFailed;
        case CoordinateReadFailure::MappingChanged:
            return CoordinateDecryptError::EntryMappingChanged;
    }
    return CoordinateDecryptError::EntryCodeReadFailed;
}

constexpr const char* CoordinateReadPathName(
    CoordinateReadPath path) noexcept {
    switch (path) {
        case CoordinateReadPath::None:
            return "NONE";
        case CoordinateReadPath::ProcessVm:
            return "PVM";
        case CoordinateReadPath::ProcMem:
            return "PMEM";
    }
    return "UNKNOWN";
}

constexpr const char* CoordinateReadStageName(
    CoordinateReadStage stage) noexcept {
    switch (stage) {
        case CoordinateReadStage::None:
            return "NONE";
        case CoordinateReadStage::Root:
            return "ROOT";
        case CoordinateReadStage::Context:
            return "CONTEXT";
        case CoordinateReadStage::Entry:
            return "ENTRY";
        case CoordinateReadStage::CodePage:
            return "CODE_PAGE";
        case CoordinateReadStage::DynamicPage:
            return "DYNAMIC_PAGE";
        case CoordinateReadStage::Parameter:
            return "PARAMETER";
        case CoordinateReadStage::PoolPointer:
            return "POOL_POINTER";
        case CoordinateReadStage::RingIndex:
            return "RING_INDEX";
        case CoordinateReadStage::Position:
            return "POSITION";
    }
    return "UNKNOWN";
}

constexpr const char* CoordinateReadFailureName(
    CoordinateReadFailure failure) noexcept {
    switch (failure) {
        case CoordinateReadFailure::None:
            return "NONE";
        case CoordinateReadFailure::InvalidRange:
            return "RANGE";
        case CoordinateReadFailure::TransportUnavailable:
            return "UNAVAILABLE";
        case CoordinateReadFailure::PermissionDenied:
            return "PERMISSION";
        case CoordinateReadFailure::AddressFault:
            return "ADDRESS";
        case CoordinateReadFailure::ShortRead:
            return "SHORT";
        case CoordinateReadFailure::ProcessVmReadFailed:
            return "PVM";
        case CoordinateReadFailure::ProcMemOpenFailed:
            return "PMEM_OPEN";
        case CoordinateReadFailure::ProcMemReadFailed:
            return "PMEM_READ";
        case CoordinateReadFailure::MappingChanged:
            return "MAPPING_CHANGED";
    }
    return "UNKNOWN";
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

inline std::string FormatCoordinateDecryptDiagnostic(
    CoordinateDecryptError error,
    int systemError,
    const CoordinateReadDiagnostic& read);

inline std::string FormatCoordinateDecryptDiagnostic(
    CoordinateDecryptError error,
    int systemError,
    const CoordinateReadDiagnostic& read,
    const CoordinatePoolPointerDiagnostic& poolPointer) {
    std::string message =
        FormatCoordinateDecryptDiagnostic(error, systemError, read);
    if (!poolPointer.HasFailure()) return message;

    std::array<char, 512> suffix{};
    if (poolPointer.read.HasFailure()) {
        std::snprintf(
            suffix.data(),
            suffix.size(),
            " POOL=CD-%04u P_SYS=%d P_STATE=0x%X P_OFF=%d "
            "P_CTX=0x%llX P_AT=0x%llX P_RAW=0x%llX P_VALUE=0x%llX "
            "P_READ=%s P_PRI=%s P_PSYS=%d P_LAST=%s P_LSYS=%d "
            "P_TRY=0x%X P_CALLS=%u",
            static_cast<unsigned int>(
                CoordinateDecryptErrorCode(poolPointer.error)),
            poolPointer.systemError,
            static_cast<unsigned int>(poolPointer.stateFlags),
            poolPointer.offset,
            static_cast<unsigned long long>(poolPointer.computedContext),
            static_cast<unsigned long long>(poolPointer.address),
            static_cast<unsigned long long>(poolPointer.rawValue),
            static_cast<unsigned long long>(poolPointer.normalizedValue),
            CoordinateReadFailureName(poolPointer.read.failure),
            CoordinateReadPathName(poolPointer.read.primaryPath),
            poolPointer.read.primarySystemError,
            CoordinateReadPathName(poolPointer.read.lastPath),
            poolPointer.read.lastSystemError,
            static_cast<unsigned int>(poolPointer.read.attemptedPaths),
            static_cast<unsigned int>(poolPointer.read.attemptCount));
    } else {
        std::snprintf(
            suffix.data(),
            suffix.size(),
            " POOL=CD-%04u P_SYS=%d P_STATE=0x%X P_OFF=%d "
            "P_CTX=0x%llX P_AT=0x%llX P_RAW=0x%llX P_VALUE=0x%llX",
            static_cast<unsigned int>(
                CoordinateDecryptErrorCode(poolPointer.error)),
            poolPointer.systemError,
            static_cast<unsigned int>(poolPointer.stateFlags),
            poolPointer.offset,
            static_cast<unsigned long long>(poolPointer.computedContext),
            static_cast<unsigned long long>(poolPointer.address),
            static_cast<unsigned long long>(poolPointer.rawValue),
            static_cast<unsigned long long>(poolPointer.normalizedValue));
    }
    message.append(suffix.data());
    return message;
}

inline std::string FormatCoordinateDecryptDiagnostic(
    CoordinateDecryptError error,
    int systemError,
    const CoordinateReadDiagnostic& read) {
    if (!read.HasFailure()) {
        return FormatCoordinateDecryptDiagnostic(error, systemError);
    }
    std::array<char, 384> message{};
    std::snprintf(
        message.data(),
        message.size(),
        "COORD CD-%04u SYS=%d READ=%s STAGE=%s "
        "PRI=%s P_SYS=%d P_DONE=%llu LAST=%s L_SYS=%d L_DONE=%llu "
        "TRY=0x%X CALLS=%u AT=0x%llX N=%llu",
        static_cast<unsigned int>(CoordinateDecryptErrorCode(error)),
        systemError,
        CoordinateReadFailureName(read.failure),
        CoordinateReadStageName(read.stage),
        CoordinateReadPathName(read.primaryPath),
        read.primarySystemError,
        static_cast<unsigned long long>(read.primaryCompleted),
        CoordinateReadPathName(read.lastPath),
        read.lastSystemError,
        static_cast<unsigned long long>(read.lastCompleted),
        static_cast<unsigned int>(read.attemptedPaths),
        static_cast<unsigned int>(read.attemptCount),
        static_cast<unsigned long long>(read.address),
        static_cast<unsigned long long>(read.size));
    return message.data();
}

}  // namespace lengjing::game
