#pragma once

#include "game/native/AlgorithmCoordinateDiagnostics.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace lengjing::game::native {

struct AlgorithmCoordinate {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct AlgorithmCoordinateRecord {
    std::uintptr_t actor = 0;
    AlgorithmCoordinate coordinate{};
    bool valid = false;
};

class AlgorithmCoordinateReader final {
public:
    // The MOV/MOVK immediate is a byte RVA; decompiler pointer scaling is not.
    static constexpr std::uintptr_t kRecordTableRva = 0x1D15B700ULL;
    static constexpr std::size_t kRecordStride = 0x20;
    static constexpr std::size_t kRecordSize = 0x20;
    static constexpr std::size_t kCoordinateOffset = 0x08;
    static constexpr std::uint32_t kMaximumRecordCount = 5000;

    template <typename ReadBytes>
    bool Read(std::uintptr_t moduleBase,
              std::uintptr_t actor,
              AlgorithmCoordinate& coordinate,
              ReadBytes&& readBytes) const {
        AlgorithmCoordinateDiagnostic diagnostic{};
        return Read(
            moduleBase,
            actor,
            coordinate,
            diagnostic,
            std::forward<ReadBytes>(readBytes));
    }

    template <typename ReadBytes>
    bool Read(std::uintptr_t moduleBase,
              std::uintptr_t actor,
              AlgorithmCoordinate& coordinate,
              AlgorithmCoordinateDiagnostic& diagnostic,
              ReadBytes&& readBytes) const {
        coordinate = AlgorithmCoordinate{};
        diagnostic = AlgorithmCoordinateDiagnostic{};
        diagnostic.actor = actor;
        if (moduleBase == 0 || actor == 0) {
            diagnostic.error = AlgorithmCoordinateReadError::InvalidInput;
            return false;
        }

        std::vector<AlgorithmCoordinateRecord> snapshot;
        if (!ReadTable(
                moduleBase,
                snapshot,
                diagnostic,
                std::forward<ReadBytes>(readBytes))) {
            diagnostic.actor = actor;
            return false;
        }

        diagnostic.actor = actor;
        for (std::uint32_t index = 0;
             index < static_cast<std::uint32_t>(snapshot.size());
             ++index) {
            const AlgorithmCoordinateRecord& record = snapshot[index];
            if (record.actor != actor) continue;
            diagnostic.recordIndex = index;
            diagnostic.x = record.coordinate.x;
            diagnostic.y = record.coordinate.y;
            diagnostic.z = record.coordinate.z;
            if (!record.valid) {
                diagnostic.error =
                    AlgorithmCoordinateReadError::CoordinateInvalid;
                return false;
            }
            coordinate = record.coordinate;
            diagnostic.error = AlgorithmCoordinateReadError::None;
            return true;
        }
        diagnostic.error = AlgorithmCoordinateReadError::ActorNotFound;
        return false;
    }

    template <typename ReadBytes>
    bool ReadTable(
        std::uintptr_t moduleBase,
        std::vector<AlgorithmCoordinateRecord>& snapshot,
        AlgorithmCoordinateDiagnostic& diagnostic,
        ReadBytes&& readBytes) const {
        snapshot.clear();
        diagnostic = AlgorithmCoordinateDiagnostic{};
        if (moduleBase == 0) {
            diagnostic.error = AlgorithmCoordinateReadError::InvalidInput;
            return false;
        }

        std::uintptr_t tableAddress = 0;
        if (!CheckedAdd(moduleBase, kRecordTableRva, tableAddress)) {
            diagnostic.error =
                AlgorithmCoordinateReadError::TableAddressOverflow;
            return false;
        }
        diagnostic.tableAddress = tableAddress;

        std::uintptr_t table = 0;
        std::uintptr_t records = 0;
        std::uint32_t count = 0;
        if (!ReadValue(readBytes, tableAddress, table)) {
            diagnostic.error =
                AlgorithmCoordinateReadError::TablePointerReadFailed;
            return false;
        }
        diagnostic.table = table;
        if (table == 0) {
            diagnostic.error = AlgorithmCoordinateReadError::TableUnavailable;
            return false;
        }
        if (!ReadValue(readBytes, table, records)) {
            diagnostic.error =
                AlgorithmCoordinateReadError::RecordsPointerReadFailed;
            return false;
        }
        diagnostic.records = records;
        if (records == 0) {
            diagnostic.error =
                AlgorithmCoordinateReadError::RecordsUnavailable;
            return false;
        }
        if (!ReadAt(readBytes, table, sizeof(std::uintptr_t), count)) {
            diagnostic.error = AlgorithmCoordinateReadError::CountReadFailed;
            return false;
        }
        diagnostic.count = count;
        if (count == 0 || count > kMaximumRecordCount) {
            diagnostic.error = AlgorithmCoordinateReadError::CountInvalid;
            return false;
        }

        std::uintptr_t snapshotSize = 0;
        if (!CheckedMultiply(count, kRecordStride, snapshotSize) ||
            snapshotSize > std::numeric_limits<std::size_t>::max() ||
            !CanRead(records, static_cast<std::size_t>(snapshotSize))) {
            diagnostic.error =
                AlgorithmCoordinateReadError::SnapshotRangeInvalid;
            return false;
        }

        std::vector<std::uint8_t> bytes;
        try {
            bytes.resize(static_cast<std::size_t>(snapshotSize));
            snapshot.reserve(count);
        } catch (const std::bad_alloc&) {
            diagnostic.error =
                AlgorithmCoordinateReadError::SnapshotAllocationFailed;
            return false;
        }
        if (!readBytes(records, bytes.data(), bytes.size())) {
            diagnostic.error =
                AlgorithmCoordinateReadError::SnapshotReadFailed;
            return false;
        }

        std::uintptr_t verifiedTable = 0;
        std::uintptr_t verifiedRecords = 0;
        std::uint32_t verifiedCount = 0;
        if (!ReadValue(readBytes, tableAddress, verifiedTable) ||
            verifiedTable != table ||
            !ReadValue(readBytes, table, verifiedRecords) ||
            verifiedRecords != records ||
            !ReadAt(
                readBytes,
                table,
                sizeof(std::uintptr_t),
                verifiedCount) ||
            verifiedCount != count) {
            diagnostic.error = AlgorithmCoordinateReadError::SnapshotChanged;
            return false;
        }

        for (std::uint32_t index = 0; index < count; ++index) {
            const std::size_t offset =
                static_cast<std::size_t>(index) * kRecordStride;
            AlgorithmCoordinateRecord record{};
            std::memcpy(
                &record.actor,
                bytes.data() + offset,
                sizeof(record.actor));
            std::memcpy(
                &record.coordinate,
                bytes.data() + offset + kCoordinateOffset,
                sizeof(record.coordinate));
            record.valid = IsValid(record.coordinate);
            if (record.actor != 0 && record.valid) {
                ++diagnostic.validCount;
            }
            snapshot.push_back(record);
        }
        diagnostic.error = AlgorithmCoordinateReadError::None;
        return true;
    }

private:
    static bool IsValid(const AlgorithmCoordinate& coordinate) noexcept {
        return IsValidAlgorithmCoordinateValue(
            coordinate.x, coordinate.y, coordinate.z);
    }

    static bool CheckedAdd(std::uintptr_t base,
                           std::uintptr_t offset,
                           std::uintptr_t& result) noexcept {
        if (offset > std::numeric_limits<std::uintptr_t>::max() - base) {
            result = 0;
            return false;
        }
        result = base + offset;
        return true;
    }

    static bool CheckedMultiply(std::uint32_t index,
                                std::size_t stride,
                                std::uintptr_t& result) noexcept {
        const auto value = static_cast<std::uintptr_t>(index);
        if (stride > std::numeric_limits<std::uintptr_t>::max() ||
            (stride != 0 && value >
                std::numeric_limits<std::uintptr_t>::max() / stride)) {
            result = 0;
            return false;
        }
        result = value * stride;
        return true;
    }

    static bool CanRead(std::uintptr_t address, std::size_t size) noexcept {
        return address != 0 && size != 0 &&
            size - 1 <= std::numeric_limits<std::uintptr_t>::max() - address;
    }

    template <typename ReadBytes, typename T>
    static bool ReadValue(ReadBytes& readBytes,
                          std::uintptr_t address,
                          T& value) {
        value = T{};
        return CanRead(address, sizeof(T)) &&
            static_cast<bool>(readBytes(address, &value, sizeof(value)));
    }

    template <typename ReadBytes, typename T>
    static bool ReadAt(ReadBytes& readBytes,
                       std::uintptr_t base,
                       std::uintptr_t offset,
                       T& value) {
        std::uintptr_t address = 0;
        return CheckedAdd(base, offset, address) &&
            ReadValue(readBytes, address, value);
    }
};

}  // namespace lengjing::game::native
