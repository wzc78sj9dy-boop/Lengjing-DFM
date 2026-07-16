#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace lengjing::game::native {

struct RemoteFieldTableLayout {
    std::uintptr_t entriesOffset = 0x220;
    std::uintptr_t countOffset = 0x228;
    std::uintptr_t slotFieldOffset = 0x0;
    std::uintptr_t fieldNameIndexOffset = 0x1C;
    std::size_t slotStride = 0x10;
    std::uint32_t maximumCount = 0x12B;
};

struct ProjectileSpeedLayout {
    std::uintptr_t weaponIdOffset = 0x838;
    std::uintptr_t identityOffset = 0x840;
    RemoteFieldTableLayout fields{};
    std::array<std::uintptr_t, 5> instanceChain{
        0x438, 0x538, 0x38, 0x170, 0xC8};
    std::uintptr_t speedOffset = 0x6C;
    float minimumSpeed = 1.0f;
    float maximumSpeed = 500000.0f;
};

class ProjectileSpeedReader final {
public:
    static constexpr std::string_view kRequiredField =
        "WeaponFuncComponentFiringAnim";

    explicit ProjectileSpeedReader(ProjectileSpeedLayout layout = {})
        : layout_(layout) {}

    void Invalidate() noexcept {
        resolutionAttempted_ = false;
        cachedIdentity_ = 0;
        currentWeaponId_ = 0;
        matchedField_.reset();
    }

    template <typename ReadBytes, typename ResolveName>
    std::optional<float> Read(std::uintptr_t weaponRoot,
                              ReadBytes&& readBytes,
                              ResolveName&& resolveName) {
        std::uintptr_t identity = 0;
        if (!ReadAt(readBytes, weaponRoot, layout_.identityOffset, identity) ||
            identity == 0) {
            Invalidate();
            return std::nullopt;
        }

        std::uint64_t weaponId = 0;
        ReadAt(readBytes, weaponRoot, layout_.weaponIdOffset, weaponId);
        currentWeaponId_ = weaponId;

        if (!resolutionAttempted_ || cachedIdentity_ != identity) {
            resolutionAttempted_ = true;
            cachedIdentity_ = identity;
            matchedField_ = FindField(
                weaponRoot, readBytes, resolveName);
        }
        if (!matchedField_.has_value()) return std::nullopt;

        std::uintptr_t instance = weaponRoot;
        for (const std::uintptr_t offset : layout_.instanceChain) {
            std::uintptr_t next = 0;
            if (!ReadAt(readBytes, instance, offset, next) || next == 0) {
                return std::nullopt;
            }
            instance = next;
        }

        float speed = 0.0f;
        if (!ReadAt(readBytes, instance, layout_.speedOffset, speed) ||
            !std::isfinite(speed) || speed < layout_.minimumSpeed ||
            speed > layout_.maximumSpeed) {
            return std::nullopt;
        }
        return speed;
    }

    std::uintptr_t CachedIdentity() const noexcept { return cachedIdentity_; }
    std::uint64_t CurrentWeaponId() const noexcept { return currentWeaponId_; }

private:
    template <typename ReadBytes, typename T>
    static bool ReadValue(ReadBytes& readBytes,
                          std::uintptr_t address,
                          T& value) {
        value = T{};
        return address != 0 &&
            static_cast<bool>(readBytes(address, &value, sizeof(value)));
    }

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
    static bool ReadAt(ReadBytes& readBytes,
                       std::uintptr_t base,
                       std::uintptr_t offset,
                       T& value) {
        std::uintptr_t address = 0;
        return CheckedAdd(base, offset, address) &&
            ReadValue(readBytes, address, value);
    }

    template <typename ReadBytes, typename ResolveName>
    std::optional<std::uintptr_t> FindField(
        std::uintptr_t owner,
        ReadBytes& readBytes,
        ResolveName& resolveName) const {
        const RemoteFieldTableLayout& fields = layout_.fields;
        if (fields.slotStride == 0 || fields.maximumCount == 0) {
            return std::nullopt;
        }

        std::uintptr_t entries = 0;
        std::uint32_t count = 0;
        if (!ReadAt(readBytes, owner, fields.entriesOffset, entries) ||
            !ReadAt(readBytes, owner, fields.countOffset, count) ||
            entries == 0 || count == 0 || count > fields.maximumCount) {
            return std::nullopt;
        }

        for (std::uint32_t index = 0; index < count; ++index) {
            const std::uintptr_t indexValue = static_cast<std::uintptr_t>(index);
            if (indexValue >
                (std::numeric_limits<std::uintptr_t>::max() - entries) /
                    fields.slotStride) {
                break;
            }
            const std::uintptr_t slot = entries + indexValue * fields.slotStride;
            std::uintptr_t field = 0;
            if (!ReadAt(readBytes, slot, fields.slotFieldOffset, field) || field == 0) {
                continue;
            }
            std::uint32_t nameIndex = 0;
            if (!ReadAt(readBytes, field, fields.fieldNameIndexOffset, nameIndex)) {
                continue;
            }
            if (std::string_view(resolveName(nameIndex)) == kRequiredField) {
                return field;
            }
        }
        return std::nullopt;
    }

    ProjectileSpeedLayout layout_{};
    bool resolutionAttempted_ = false;
    std::uintptr_t cachedIdentity_ = 0;
    std::uint64_t currentWeaponId_ = 0;
    std::optional<std::uintptr_t> matchedField_{};
};

}  // namespace lengjing::game::native
