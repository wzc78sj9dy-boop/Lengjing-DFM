#include "test_support.h"

#include "game/GameVersionPolicy.h"

#include <memory>

void RunGameVersionPolicyTests() {
    using namespace lengjing;

    REQUIRE(game::ResolveGameVersionPackage(0) ==
            game::kGameVersionPackages[0]);
    REQUIRE(game::ResolveGameVersionPackage(1) ==
            game::kGameVersionPackages[1]);
    REQUIRE(game::ResolveGameVersionPackage(2) ==
            game::kGameVersionPackages[2]);
    REQUIRE(game::kGameVersionPackages[0] != game::kGameVersionPackages[1]);
    REQUIRE(game::ResolveGameVersionPackage(-1).empty());
    REQUIRE(game::ResolveGameVersionPackage(3).empty());

    const auto cloudLayout =
        std::make_shared<auth::CloudLayoutDocument>();
    cloudLayout->identity.packageName =
        game::ResolveGameVersionPackage(0);

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

    cloudLayout->identity.packageName =
        game::ResolveGameVersionPackage(1);
    REQUIRE(game::SelectCloudLayoutForGameVersion(
                cloudLayout, 1) == cloudLayout);
    REQUIRE(game::SelectCloudLayoutForGameVersion(
                cloudLayout, 2) == nullptr);

    cloudLayout->identity.packageName =
        game::ResolveGameVersionPackage(2);
    REQUIRE(game::SelectCloudLayoutForGameVersion(
                cloudLayout, 2) == cloudLayout);
    REQUIRE(game::SelectCloudLayoutForGameVersion(
                cloudLayout, 0) == nullptr);

}
