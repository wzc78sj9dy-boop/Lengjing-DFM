#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace lengjing::game::native {

class IndexedCoordinateReader final {
public:
    using Coordinate = std::array<float, 3>;

    template <typename ReadBytes>
    static bool Read(std::uintptr_t subject,
                     Coordinate& coordinate,
                     ReadBytes&& readBytes) {
        coordinate = Coordinate{};
        if (subject == 0) return false;

        std::uintptr_t current = subject;
        constexpr std::array<std::uintptr_t, 5> links{
            0x3D0, 0xF8, 0x8, 0x4C8, 0x118};
        for (const std::uintptr_t offset : links) {
            std::uintptr_t next = 0;
            if (!ReadAt(readBytes, current, offset, next) || next == 0) {
                return false;
            }
            current = next;
        }

        constexpr std::uintptr_t kRecordStride = 0x70;
        constexpr std::uintptr_t kCoordinatePointerOffset = 0x30;
        constexpr std::size_t kRecordCount = 1001;
        for (std::size_t index = 0; index < kRecordCount; ++index) {
            if (index >
                (std::numeric_limits<std::uintptr_t>::max() - current) /
                    kRecordStride) {
                return false;
            }
            const std::uintptr_t record = current + index * kRecordStride;
            std::uintptr_t candidate = 0;
            if (!ReadValue(readBytes, record, candidate) || candidate != subject) {
                continue;
            }
            std::uintptr_t coordinateAddress = 0;
            if (!ReadAt(
                    readBytes,
                    record,
                    kCoordinatePointerOffset,
                    coordinateAddress) ||
                coordinateAddress == 0) {
                continue;
            }
            return ReadValue(readBytes, coordinateAddress, coordinate);
        }
        return false;
    }

private:
    static bool CheckedAdd(std::uintptr_t base,
                           std::uintptr_t offset,
                           std::uintptr_t& result) noexcept {
        if (base == 0 || offset > std::numeric_limits<std::uintptr_t>::max() - base) {
            result = 0;
            return false;
        }
        result = base + offset;
        return true;
    }

    template <typename ReadBytes, typename T>
    static bool ReadValue(ReadBytes& readBytes,
                          std::uintptr_t address,
                          T& value) {
        value = T{};
        return address != 0 &&
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
