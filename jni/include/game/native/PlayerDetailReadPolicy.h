#pragma once

#include <cstdint>

namespace lengjing::game::native {

enum class PlayerDetailReadField : std::uint16_t {
    CoreHealth = 1u << 0,
    Downed = 1u << 1,
    HelmetLevel = 1u << 2,
    ArmorLevel = 1u << 3,
    HelmetDurability = 1u << 4,
    ArmorDurability = 1u << 5,
};

using PlayerDetailReadMask = std::uint16_t;

constexpr PlayerDetailReadMask PlayerDetailReadBit(
    PlayerDetailReadField field) noexcept {
    return static_cast<PlayerDetailReadMask>(field);
}

constexpr bool HasPlayerDetailReadField(
    PlayerDetailReadMask mask,
    PlayerDetailReadField field) noexcept {
    return (mask & PlayerDetailReadBit(field)) != 0;
}

constexpr bool HasAnyPlayerDetailReadField(
    PlayerDetailReadMask mask,
    PlayerDetailReadMask fields) noexcept {
    return (mask & fields) != 0;
}

constexpr PlayerDetailReadMask kPlayerCoreHealthReadMask =
    PlayerDetailReadBit(PlayerDetailReadField::CoreHealth);

constexpr PlayerDetailReadMask kPlayerDownedReadMask =
    PlayerDetailReadBit(PlayerDetailReadField::Downed);

constexpr PlayerDetailReadMask kPlayerEquipmentLevelReadMask =
    PlayerDetailReadBit(PlayerDetailReadField::HelmetLevel) |
    PlayerDetailReadBit(PlayerDetailReadField::ArmorLevel);

constexpr PlayerDetailReadMask kPlayerEquipmentDurabilityReadMask =
    PlayerDetailReadBit(PlayerDetailReadField::HelmetDurability) |
    PlayerDetailReadBit(PlayerDetailReadField::ArmorDurability);

constexpr PlayerDetailReadMask ResolvePlayerCoreHealthReadMask(
    bool coordinateAvailable) noexcept {
    return coordinateAvailable ? kPlayerCoreHealthReadMask : 0;
}

constexpr PlayerDetailReadMask ResolvePlayerDownedReadMask(
    bool coreHealthAvailable,
    float health) noexcept {
    return coreHealthAvailable && health <= 0.0f
        ? kPlayerDownedReadMask
        : 0;
}

struct PlayerEquipmentReadRequest {
    bool finalDrawable = false;
    bool bot = false;
    bool healthBar = false;
    bool armorLevel = false;
    bool armorDurability = false;
};

constexpr PlayerDetailReadMask ResolvePlayerEquipmentReadMask(
    const PlayerEquipmentReadRequest& request) noexcept {
    if (!request.finalDrawable) {
        return 0;
    }

    PlayerDetailReadMask mask = request.healthBar
        ? PlayerDetailReadBit(PlayerDetailReadField::ArmorDurability)
        : 0;
    if (request.bot) {
        return mask;
    }
    if (request.armorLevel) {
        mask |= kPlayerEquipmentLevelReadMask;
    }
    if (request.armorDurability) {
        mask |= kPlayerEquipmentDurabilityReadMask;
    }
    return mask;
}

}  // namespace lengjing::game::native
