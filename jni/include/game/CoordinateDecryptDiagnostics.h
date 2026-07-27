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
    MemoryTransportUnavailable = 2001,
    EngineSetupFailed = 5001,
    SampleUnavailable = 5002,
    RecordBaseUnavailable = 5010,
    PositionReadFailed = 5012,
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
