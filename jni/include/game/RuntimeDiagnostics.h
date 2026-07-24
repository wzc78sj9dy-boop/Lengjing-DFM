#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

namespace lengjing::game {

enum class RuntimeError : std::uint16_t {
    None = 0,

    InvalidConfiguration = 1001,
    StartRejected = 1002,
    BackendCloseFailed = 1003,
    TargetProcessNotFound = 1101,

    MemoryModeInvalid = 1201,
    KernelDriverSetupFailed = 1202,
    KernelInterfaceCreateFailed = 1203,

    ModuleBaseUnavailable = 1301,
    ModuleReadFailed = 1302,
    CloudBuildIdMismatch = 1401,
    CloudLayoutInvalid = 1402,
    InputChannelStartFailed = 1501,

    BackendUnavailable = 2001,
    TargetProcessExited = 2002,
    ModuleAddressInvalid = 2101,
    WorldEntryInvalid = 2102,
    WorldUnavailable = 2103,
    LevelUnavailable = 2104,
    ActorSourceUnavailable = 2105,
    LocalViewUnavailable = 2106,
    CameraUnavailable = 2107,
    WorldTransition = 2108,
    ActorListUnavailable = 2201,
    NamePoolUnavailable = 2202,
    DecodedActorSourceUnavailable = 2203,
    DecodedActorRecordsUnavailable = 2204,

    BackendOpenFailed = 9001,
    BackendReadFailed = 9002,
    FrameDataUnavailable = 9003,
    StandardException = 9004,
    UnknownException = 9005,
};

struct RuntimeDiagnostic {
    RuntimeError error = RuntimeError::None;
    int systemError = 0;
};

constexpr std::uint16_t RuntimeErrorCode(RuntimeError error) noexcept {
    return static_cast<std::uint16_t>(error);
}

inline std::string FormatRuntimeDiagnostic(
    RuntimeError error,
    int systemError) {
    std::array<char, 40> message{};
    std::snprintf(
        message.data(),
        message.size(),
        "RUNTIME RT-%04u SYS=%d",
        static_cast<unsigned int>(RuntimeErrorCode(error)),
        systemError);
    return message.data();
}

}  // namespace lengjing::game
