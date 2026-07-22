#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

namespace lengjing::game::native {

enum class AlgorithmCoordinateReadError : std::uint16_t {
    None = 0,
    InvalidInput,
    TableAddressOverflow,
    TablePointerReadFailed,
    TableUnavailable,
    RecordsPointerReadFailed,
    RecordsUnavailable,
    CountReadFailed,
    CountInvalid,
    SnapshotRangeInvalid,
    SnapshotAllocationFailed,
    SnapshotReadFailed,
    SnapshotChanged,
    ActorNotFound,
    CoordinateInvalid,
};

enum class AlgorithmCoordinateSource : std::uint8_t {
    None = 0,
    RuntimeObject,
    RecordTable,
};

inline constexpr std::uint32_t kInvalidAlgorithmCoordinateRecordIndex =
    std::numeric_limits<std::uint32_t>::max();

struct AlgorithmCoordinateDiagnostic {
    AlgorithmCoordinateReadError error =
        AlgorithmCoordinateReadError::None;
    std::uintptr_t tableAddress = 0;
    std::uintptr_t table = 0;
    std::uintptr_t records = 0;
    std::uintptr_t actor = 0;
    std::uint32_t count = 0;
    std::uint32_t validCount = 0;
    std::uint32_t recordIndex = kInvalidAlgorithmCoordinateRecordIndex;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

constexpr bool operator==(const AlgorithmCoordinateDiagnostic& left,
                          const AlgorithmCoordinateDiagnostic& right) noexcept {
    return left.error == right.error &&
        left.tableAddress == right.tableAddress &&
        left.table == right.table && left.records == right.records &&
        left.actor == right.actor && left.count == right.count &&
        left.validCount == right.validCount &&
        left.recordIndex == right.recordIndex && left.x == right.x &&
        left.y == right.y && left.z == right.z;
}

constexpr bool operator!=(const AlgorithmCoordinateDiagnostic& left,
                          const AlgorithmCoordinateDiagnostic& right) noexcept {
    return !(left == right);
}

constexpr std::uint16_t AlgorithmCoordinateReadErrorCode(
    AlgorithmCoordinateReadError error) noexcept {
    return static_cast<std::uint16_t>(error);
}

inline bool IsValidAlgorithmCoordinateValue(
    float x, float y, float z) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        return false;
    }
    if (x != 0.0f || y != 0.0f) return true;
    return z != 0.0f && z != -90.0f;
}

inline std::string FormatAlgorithmCoordinateDiagnostic(
    const AlgorithmCoordinateDiagnostic& diagnostic) {
    char buffer[24]{};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "ALGO AC-%04u",
        static_cast<unsigned>(
            AlgorithmCoordinateReadErrorCode(diagnostic.error)));
    return buffer;
}

}
