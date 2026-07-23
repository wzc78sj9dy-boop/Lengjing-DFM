#include "game/native/PlayerDetailReadPolicy.h"
#include "test_support.h"

void RunPlayerDetailReadPolicyTests() {
    using lengjing::game::native::HasPlayerDetailReadField;
    using lengjing::game::native::HasAnyPlayerDetailReadField;
    using lengjing::game::native::kPlayerCoreHealthReadMask;
    using lengjing::game::native::kPlayerDownedReadMask;
    using lengjing::game::native::kPlayerEquipmentDurabilityReadMask;
    using lengjing::game::native::kPlayerEquipmentLevelReadMask;
    using lengjing::game::native::PlayerDetailReadBit;
    using lengjing::game::native::PlayerDetailReadField;
    using lengjing::game::native::PlayerEquipmentReadRequest;
    using lengjing::game::native::ResolvePlayerCoreHealthReadMask;
    using lengjing::game::native::ResolvePlayerDownedReadMask;
    using lengjing::game::native::ResolvePlayerEquipmentReadMask;

    REQUIRE(ResolvePlayerCoreHealthReadMask(false) == 0);
    REQUIRE(ResolvePlayerCoreHealthReadMask(true) ==
            kPlayerCoreHealthReadMask);

    REQUIRE(ResolvePlayerDownedReadMask(false, 0.0f) == 0);
    REQUIRE(ResolvePlayerDownedReadMask(true, 100.0f) == 0);
    REQUIRE(ResolvePlayerDownedReadMask(true, 0.01f) == 0);
    REQUIRE(ResolvePlayerDownedReadMask(true, 0.0f) ==
            kPlayerDownedReadMask);
    REQUIRE(ResolvePlayerDownedReadMask(true, -1.0f) ==
            kPlayerDownedReadMask);

    PlayerEquipmentReadRequest request{
        false,
        false,
        true,
        true,
        true,
    };
    REQUIRE(ResolvePlayerEquipmentReadMask(request) == 0);

    request.finalDrawable = true;
    request.healthBar = false;
    request.armorLevel = false;
    request.armorDurability = false;
    REQUIRE(ResolvePlayerEquipmentReadMask(request) == 0);

    request.healthBar = true;
    REQUIRE(ResolvePlayerEquipmentReadMask(request) ==
            PlayerDetailReadBit(
                PlayerDetailReadField::ArmorDurability));

    request.healthBar = false;
    request.armorLevel = true;
    REQUIRE(ResolvePlayerEquipmentReadMask(request) ==
            kPlayerEquipmentLevelReadMask);

    request.armorLevel = false;
    request.armorDurability = true;
    REQUIRE(ResolvePlayerEquipmentReadMask(request) ==
            kPlayerEquipmentDurabilityReadMask);

    request.healthBar = true;
    request.armorLevel = true;
    REQUIRE(ResolvePlayerEquipmentReadMask(request) ==
            (kPlayerEquipmentDurabilityReadMask |
             kPlayerEquipmentLevelReadMask));

    request.bot = true;
    request.healthBar = false;
    REQUIRE(ResolvePlayerEquipmentReadMask(request) == 0);

    request.healthBar = true;
    const auto botHealthBarMask =
        ResolvePlayerEquipmentReadMask(request);
    REQUIRE(botHealthBarMask ==
            PlayerDetailReadBit(
                PlayerDetailReadField::ArmorDurability));
    REQUIRE(HasPlayerDetailReadField(
        botHealthBarMask, PlayerDetailReadField::ArmorDurability));
    REQUIRE(!HasPlayerDetailReadField(
        botHealthBarMask, PlayerDetailReadField::HelmetDurability));
    REQUIRE(!HasPlayerDetailReadField(
        botHealthBarMask, PlayerDetailReadField::HelmetLevel));
    REQUIRE(!HasPlayerDetailReadField(
        botHealthBarMask, PlayerDetailReadField::ArmorLevel));
    REQUIRE(HasAnyPlayerDetailReadField(
        botHealthBarMask, kPlayerEquipmentDurabilityReadMask));
    REQUIRE(!HasAnyPlayerDetailReadField(
        botHealthBarMask, kPlayerEquipmentLevelReadMask));
}
