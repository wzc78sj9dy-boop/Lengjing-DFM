#include "test_support.h"

#include "game/GameVersionPolicy.h"

#include <memory>

void RunGameVersionPolicyTests() {
    using namespace lengjing;

    REQUIRE(game::ResolveGameVersionPackage(0) ==
            "com.tencent.tmgp.dfm");
    REQUIRE(game::ResolveGameVersionPackage(1) == "com.proxima.dfm");
    REQUIRE(game::ResolveGameVersionPackage(2) ==
            "com.garena.game.df");
    REQUIRE(game::ResolveGameVersionPackage(-1).empty());
    REQUIRE(game::ResolveGameVersionPackage(3).empty());

    const auto cloudLayout =
        std::make_shared<auth::CloudLayoutDocument>();
    cloudLayout->identity.packageName = "com.tencent.tmgp.dfm";

    REQUIRE(game::CloudLayoutMatchesGameVersion(
        cloudLayout.get(), 0));
    REQUIRE(!game::CloudLayoutMatchesGameVersion(
        cloudLayout.get(), 1));
    REQUIRE(!game::CloudLayoutMatchesGameVersion(
        cloudLayout.get(), 2));
    REQUIRE(!game::CloudLayoutMatchesGameVersion(nullptr, 0));

    REQUIRE(game::SelectCloudLayoutForGameVersion(
                cloudLayout, 0) == cloudLayout);
    REQUIRE(game::SelectCloudLayoutForGameVersion(
                cloudLayout, 1) == nullptr);

    cloudLayout->identity.packageName = "com.proxima.dfm";
    REQUIRE(game::SelectCloudLayoutForGameVersion(
                cloudLayout, 1) == cloudLayout);
    REQUIRE(game::SelectCloudLayoutForGameVersion(
                cloudLayout, 2) == nullptr);

    cloudLayout->identity.packageName = "com.garena.game.df";
    REQUIRE(game::SelectCloudLayoutForGameVersion(
                cloudLayout, 2) == cloudLayout);
    REQUIRE(game::SelectCloudLayoutForGameVersion(
                cloudLayout, 0) == nullptr);

    const auto decrypt2Layout =
        std::make_shared<auth::CoordinatePoolCloudLayoutDocument>();
    decrypt2Layout->identity.packageName = "com.tencent.tmgp.dfm";
    REQUIRE(game::CoordinatePoolCloudLayoutMatchesGameVersion(
        decrypt2Layout.get(), 0));
    REQUIRE(!game::CoordinatePoolCloudLayoutMatchesGameVersion(
        decrypt2Layout.get(), 1));
    REQUIRE(game::SelectCoordinatePoolCloudLayoutForGameVersion(
                decrypt2Layout, 0) == decrypt2Layout);
    REQUIRE(game::SelectCoordinatePoolCloudLayoutForGameVersion(
                decrypt2Layout, 2) == nullptr);
}
