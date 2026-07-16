#include "test_support.h"

#include "game/data/ThreatCatalog.h"

#include <string_view>

void RunThreatCatalogTests() {
    using lengjing::game::data::FindThreatObject;
    using lengjing::game::data::kThreatObjects;

    REQUIRE(kThreatObjects.size() == 34);

    const auto* grenade =
        FindThreatObject("BP_ThrowableProjectileDefault_C_12");
    REQUIRE(grenade != nullptr);
    REQUIRE(grenade->displayName == std::string_view("手雷"));
    REQUIRE(grenade->radiusCentimeters == 500.0f);
    REQUIRE(grenade->red == 255);
    REQUIRE(grenade->green == 0);

    const auto* smoke =
        FindThreatObject("NC_AbilityBullet_C101_Smoke_Projectile_C");
    REQUIRE(smoke != nullptr);
    REQUIRE(smoke->displayName == std::string_view("蜂巢科技烟雾弹"));
    REQUIRE(smoke->radiusCentimeters == 400.0f);
    REQUIRE(smoke->red == 180);
    REQUIRE(smoke->green == 180);
    REQUIRE(smoke->blue == 180);

    REQUIRE(FindThreatObject("NC_BP_DFMCharacter_C") == nullptr);
}
