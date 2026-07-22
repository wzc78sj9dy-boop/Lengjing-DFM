#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace lengjing::game::native {

struct AlgorithmCoordinate {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

class AlgorithmCoordinateReader final {
public:
    static constexpr std::uintptr_t kRecordTableRva = 0x3A2B6E0ULL;
    static constexpr std::size_t kRecordStride = 0x20;
    static constexpr std::size_t kRecordSize = 0x20;
    static constexpr std::size_t kCoordinateOffset = 0x08;
    static constexpr std::uint32_t kMaximumRecordCount = 5000;

    template <typename ReadBytes>
    bool Read(std::uintptr_t moduleBase,
              std::uintptr_t actor,
              AlgorithmCoordinate& coordinate,
              ReadBytes&& readBytes) const {
        coordinate = AlgorithmCoordinate{};
        if (moduleBase == 0 || actor == 0) return false;

        std::uintptr_t tableAddress = 0;
        if (!CheckedAdd(moduleBase, kRecordTableRva, tableAddress)) {
            return false;
        }

        std::uintptr_t table = 0;
        std::uintptr_t records = 0;
        std::uint32_t count = 0;
        if (!ReadValue(readBytes, tableAddress, table) || table == 0 ||
            !ReadValue(readBytes, table, records) || records == 0 ||
            !ReadAt(readBytes, table, sizeof(std::uintptr_t), count) ||
            count == 0 || count > kMaximumRecordCount) {
            return false;
        }

        std::array<std::uint8_t, kRecordSize> record{};
        for (std::uint32_t index = 0; index < count; ++index) {
            std::uintptr_t offset = 0;
            std::uintptr_t address = 0;
            if (!CheckedMultiply(index, kRecordStride, offset) ||
                !CheckedAdd(records, offset, address) ||
                !CanRead(address, record.size())) {
                return false;
            }
            record.fill(0);
            if (!readBytes(address, record.data(), record.size())) {
                return false;
            }

            std::uintptr_t recordActor = 0;
            std::memcpy(&recordActor, record.data(), sizeof(recordActor));
            if (recordActor != actor) continue;

            AlgorithmCoordinate candidate{};
            std::memcpy(
                &candidate, record.data() + kCoordinateOffset,
                sizeof(candidate));
            if (!IsValid(candidate)) return false;
            coordinate = candidate;
            return true;
        }
        return false;
    }

private:
    static bool IsValid(const AlgorithmCoordinate& coordinate) noexcept {
        if (!std::isfinite(coordinate.x) || !std::isfinite(coordinate.y) ||
            !std::isfinite(coordinate.z)) {
            return false;
        }
        if (coordinate.x != 0.0f || coordinate.y != 0.0f) return true;
        return coordinate.z != 0.0f && coordinate.z != -90.0f;
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
