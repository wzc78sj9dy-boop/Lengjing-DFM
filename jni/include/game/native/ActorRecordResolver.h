#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>

namespace lengjing::game::native {

struct ActorRecordLayout {
    std::uintptr_t taggedContainerOffset = 0x0EEBDB14ULL;
    std::uintptr_t plainArrayOffset = 0x1D0A6908ULL;
    std::uintptr_t plainRootOffset = 0x180;
    std::uintptr_t plainMeshOffset = 0x3D0;
    std::uint32_t encryptedRecordCount = 1000;
    std::uint32_t plainRecordStride = 24;
    std::int32_t maximumPlainCount = 10000;
    std::int32_t fallbackPlainCount = 3000;
};

constexpr bool HasConfiguredActorRecordSource(
    const ActorRecordLayout& layout) noexcept {
    return layout.taggedContainerOffset != 0 || layout.plainArrayOffset != 0;
}

struct ActorArrayDescriptor {
    std::uintptr_t data = 0;
    std::uint32_t count = 0;
    std::uint32_t stride = 0;
    bool encrypted = false;
};

struct ActorAddressRecord {
    std::uintptr_t actor = 0;
    std::uintptr_t root = 0;
    std::uintptr_t mesh = 0;
};

class ActorRecordResolver final {
public:
    static constexpr std::uintptr_t kPointerPayloadMask =
        static_cast<std::uintptr_t>(0x00FFFFFFFFFFFFFFULL);

    explicit ActorRecordResolver(ActorRecordLayout layout = {})
        : layout_(layout) {}

    const ActorRecordLayout& Layout() const noexcept { return layout_; }

    static std::uintptr_t UntagPointer(std::uintptr_t value) noexcept {
        return value & kPointerPayloadMask;
    }

    template <typename ReadBytes, typename ValidatePointer>
    std::uintptr_t ResolveContainer(std::uintptr_t moduleBase,
                                    ReadBytes&& readBytes,
                                    ValidatePointer&& validatePointer) const {
        if (moduleBase == 0 || layout_.taggedContainerOffset == 0) return 0;

        std::uintptr_t entryAddress = 0;
        if (!CheckedAdd(
                moduleBase, layout_.taggedContainerOffset, entryAddress)) {
            return 0;
        }

        std::uintptr_t tagged = 0;
        if (!ReadValue(readBytes, entryAddress, tagged)) return 0;
        const std::uintptr_t encrypted = UntagPointer(tagged);
        if (!static_cast<bool>(validatePointer(encrypted)) || encrypted < 8) {
            return 0;
        }

        std::uintptr_t first = 0;
        std::uintptr_t second = 0;
        std::uintptr_t third = 0;
        std::uintptr_t result = 0;
        ReadValue(readBytes, encrypted - 8, first);
        ReadAt(readBytes, first, 16, second);
        ReadAt(readBytes, second, 16, third);
        ReadValue(readBytes, third, result);
        return UntagPointer(result);
    }

    template <typename ReadBytes>
    std::optional<std::uint32_t> FindArrayStartOffset(
        std::uintptr_t container,
        std::uintptr_t moduleBase,
        ReadBytes&& readBytes) const {
        if (container == 0 || moduleBase == 0) return std::nullopt;
        for (std::uint32_t offset = 0; offset < 512; offset += 8) {
            std::uintptr_t candidate = 0;
            if (ReadAt(readBytes, container, offset, candidate) &&
                candidate == moduleBase) {
                return offset + 80;
            }
        }
        return std::nullopt;
    }

    template <typename ReadBytes>
    int ProbeEncryptedArray(std::uintptr_t container,
                            std::uintptr_t moduleBase,
                            ReadBytes&& readBytes) const {
        const auto compare = [](const void* left,
                                const void* right,
                                std::size_t size) {
            return std::memcmp(left, right, size);
        };
        return ProbeEncryptedArrayWithCompare(
            container, moduleBase, readBytes, compare);
    }

    template <typename ReadBytes, typename Compare>
    int ProbeEncryptedArrayWithCompare(std::uintptr_t container,
                                       std::uintptr_t moduleBase,
                                       ReadBytes&& readBytes,
                                       Compare&& compare) const {
        const std::uint32_t readOffset =
            FindArrayStartOffset(container, moduleBase, readBytes).value_or(0);
        std::array<std::uint8_t, 256> block{};
        const std::array<std::uint8_t, 256> zero{};
        std::uintptr_t blockAddress = 0;
        if (CheckedAdd(container, readOffset, blockAddress)) {
            static_cast<void>(
                readBytes(blockAddress, block.data(), block.size()));
        }
        return static_cast<int>(
            compare(zero.data(), block.data(), block.size()));
    }

    template <typename ReadBytes>
    std::optional<std::uint32_t> DiscoverStride(
        std::uintptr_t actorArray,
        bool encrypted,
        ReadBytes&& readBytes) const {
        if (!encrypted) {
            return layout_.plainRecordStride == 0
                ? std::nullopt
                : std::optional<std::uint32_t>(layout_.plainRecordStride);
        }
        if (actorArray == 0) return std::nullopt;

        std::uintptr_t firstMarker = 0;
        if (!ReadAt(readBytes, actorArray, 8, firstMarker) ||
            !IsMarker(firstMarker)) {
            return std::nullopt;
        }
        for (std::uint32_t cursor = 136; cursor < 504; cursor += 8) {
            std::uintptr_t marker = 0;
            std::uintptr_t markerOffset = 0;
            if (!CheckedAdd(cursor, 16, markerOffset)) return std::nullopt;
            if (ReadAt(readBytes, actorArray, markerOffset, marker) &&
                IsMarker(marker)) {
                return cursor + 8;
            }
        }
        return std::nullopt;
    }

    template <typename ReadBytes, typename ValidatePointer>
    std::optional<ActorArrayDescriptor> Locate(
        std::uintptr_t moduleBase,
        ReadBytes&& readBytes,
        ValidatePointer&& validatePointer) const {
        const auto compare = [](const void* left,
                                const void* right,
                                std::size_t size) {
            return std::memcmp(left, right, size);
        };
        return LocateWithCompare(
            moduleBase, readBytes, validatePointer, compare);
    }

    template <typename ReadBytes, typename ValidatePointer, typename Compare>
    std::optional<ActorArrayDescriptor> LocateWithCompare(
        std::uintptr_t moduleBase,
        ReadBytes&& readBytes,
        ValidatePointer&& validatePointer,
        Compare&& compare) const {
        if (moduleBase == 0) return std::nullopt;

        const std::uintptr_t container = ResolveContainer(
            moduleBase, readBytes, validatePointer);
        if (container != 0 && layout_.encryptedRecordCount != 0 &&
            ProbeEncryptedArrayWithCompare(
                container, moduleBase, readBytes, compare) != 0) {
            const std::uint32_t startOffset =
                FindArrayStartOffset(container, moduleBase, readBytes)
                    .value_or(0);
            std::uintptr_t data = 0;
            if (CheckedAdd(container, startOffset, data)) {
                const auto stride = DiscoverStride(data, true, readBytes);
                if (stride.has_value()) {
                    return ActorArrayDescriptor{
                        data,
                        layout_.encryptedRecordCount,
                        *stride,
                        true,
                    };
                }
            }
        }

        if (layout_.plainArrayOffset == 0 ||
            layout_.plainRecordStride == 0) {
            return std::nullopt;
        }
        std::uintptr_t header = 0;
        if (!CheckedAdd(moduleBase, layout_.plainArrayOffset, header)) {
            return std::nullopt;
        }
        std::uintptr_t data = 0;
        std::int32_t count = 0;
        if (!ReadValue(readBytes, header, data) || data == 0 ||
            !ReadAt(readBytes, header, 8, count)) {
            return std::nullopt;
        }
        if (count < 1 || count > layout_.maximumPlainCount) {
            count = layout_.fallbackPlainCount;
        }
        if (count < 1) return std::nullopt;
        return ActorArrayDescriptor{
            data,
            static_cast<std::uint32_t>(count),
            layout_.plainRecordStride,
            false,
        };
    }

    template <typename ReadBytes>
    std::optional<ActorAddressRecord> ReadRecord(
        const ActorArrayDescriptor& array,
        std::uint32_t index,
        ReadBytes&& readBytes) const {
        if (array.data == 0 || array.stride == 0 || index >= array.count) {
            return std::nullopt;
        }
        const std::uintptr_t indexValue = static_cast<std::uintptr_t>(index);
        if (indexValue >
            (std::numeric_limits<std::uintptr_t>::max() - array.data) /
                array.stride) {
            return std::nullopt;
        }
        const std::uintptr_t entry =
            array.data + indexValue * array.stride;

        ActorAddressRecord record{};
        if (!ReadValue(readBytes, entry, record.actor) || record.actor == 0) {
            return std::nullopt;
        }
        if (array.encrypted) {
            ReadAt(readBytes, entry, 16, record.root);
            ReadAt(readBytes, entry, 32, record.mesh);
        } else {
            ReadAt(
                readBytes, record.actor, layout_.plainRootOffset, record.root);
            ReadAt(
                readBytes, record.actor, layout_.plainMeshOffset, record.mesh);
        }
        if (record.root == 0) return std::nullopt;
        return record;
    }

private:
    static bool IsMarker(std::uintptr_t value) noexcept {
        return value == 1 || value == 256;
    }

    static bool CheckedAdd(std::uintptr_t base,
                           std::uintptr_t offset,
                           std::uintptr_t& result) noexcept {
        if (base == 0 ||
            offset > std::numeric_limits<std::uintptr_t>::max() - base) {
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

    ActorRecordLayout layout_{};
};

}  // namespace lengjing::game::native
