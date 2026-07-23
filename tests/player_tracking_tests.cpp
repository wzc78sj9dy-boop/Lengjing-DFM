#include "test_support.h"

#include "game/native/PlayerTrackingPolicy.h"

void RunPlayerTrackingTests() {
    using lengjing::game::native::HasComparablePlayerTeams;
    using lengjing::game::native::HasUsablePlayerState;
    using lengjing::game::native::IsEnemyEligible;
    using lengjing::game::native::IsPlayerTeammate;
    using lengjing::game::native::IsSamePlayerTeam;
    using lengjing::game::native::IsWithinOffscreenWarningRange;
    using lengjing::game::native::IsWithinPlayerDrawRange;
    using lengjing::game::native::PlayerLifecycleDisposition;
    using lengjing::game::native::ResolvePlayerIdentity;
    using lengjing::game::native::ResolvePlayerLifecycleDisposition;
    using lengjing::game::native::ResolvePlayerTeam;
    using lengjing::game::native::ResolveDownedState;

    REQUIRE(ResolvePlayerLifecycleDisposition(
        true, true, 100.0f, false, false, false, false) ==
        PlayerLifecycleDisposition::Visual);
    REQUIRE(ResolvePlayerLifecycleDisposition(
        true, true, 0.0f, true, true, false, false) ==
        PlayerLifecycleDisposition::Visual);
    REQUIRE(ResolvePlayerLifecycleDisposition(
        true, true, 0.0f, true, false, true, false) ==
        PlayerLifecycleDisposition::AimOnly);
    REQUIRE(ResolvePlayerLifecycleDisposition(
        true, true, 0.0f, true, false, false, false) ==
        PlayerLifecycleDisposition::Excluded);
    REQUIRE(ResolvePlayerLifecycleDisposition(
        true, true, 0.0f, true, false, true, true) ==
        PlayerLifecycleDisposition::Excluded);
    REQUIRE(ResolvePlayerLifecycleDisposition(
        true, true, 0.0f, false, true, true, false) ==
        PlayerLifecycleDisposition::Excluded);
    REQUIRE(ResolvePlayerLifecycleDisposition(
        false, true, 100.0f, false, true, true, false) ==
        PlayerLifecycleDisposition::Excluded);
    REQUIRE(ResolvePlayerLifecycleDisposition(
        true, false, 100.0f, false, true, true, false) ==
        PlayerLifecycleDisposition::Excluded);

    REQUIRE(HasUsablePlayerState(true, false));
    REQUIRE(HasUsablePlayerState(false, true));
    REQUIRE(!HasUsablePlayerState(false, false));

    REQUIRE(ResolvePlayerTeam(true, 7, false) == 7);
    REQUIRE(ResolvePlayerTeam(false, 7, false) == -1);
    REQUIRE(ResolvePlayerTeam(false, -1, true) == 0);
    REQUIRE(HasComparablePlayerTeams(1, 2));
    REQUIRE(!HasComparablePlayerTeams(-1, 2));
    REQUIRE(IsSamePlayerTeam(3, 3));
    REQUIRE(!IsSamePlayerTeam(3, 4));
    REQUIRE(IsPlayerTeammate(3, 3, false, 3, 4, 5, 6));
    REQUIRE(IsPlayerTeammate(5, 6, true, 3, 3, 5, 6));
    REQUIRE(IsPlayerTeammate(5, 6, true, 3, 4, 5, 5));
    REQUIRE(!IsPlayerTeammate(5, 6, false, 3, 3, 5, 6));
    REQUIRE(!IsPlayerTeammate(-1, -1, true, -1, -1, -1, -1));
    REQUIRE(!IsEnemyEligible(3, 3, false));
    REQUIRE(!IsEnemyEligible(3, 3, true));
    REQUIRE(IsEnemyEligible(3, 4, false));
    REQUIRE(IsEnemyEligible(3, 4, true));
    REQUIRE(IsEnemyEligible(3, -1, false));
    REQUIRE(IsEnemyEligible(3, -1, true));
    REQUIRE(IsEnemyEligible(-1, -1, false));
    REQUIRE(IsEnemyEligible(-1, -1, true));

    REQUIRE(IsWithinPlayerDrawRange(100.0f, 100));
    REQUIRE(!IsWithinPlayerDrawRange(100.1f, 100));
    REQUIRE(IsWithinPlayerDrawRange(1000.0f, 0));
    REQUIRE(!IsWithinPlayerDrawRange(-1.0f, 100));
    REQUIRE(IsWithinOffscreenWarningRange(80.0f, 100, 200.0f));
    REQUIRE(!IsWithinOffscreenWarningRange(150.0f, 100, 200.0f));
    REQUIRE(!IsWithinOffscreenWarningRange(150.0f, 200, 100.0f));
    REQUIRE(IsWithinOffscreenWarningRange(150.0f, 0, 200.0f));

    constexpr std::uintptr_t actor = 0x100000;
    constexpr std::uintptr_t state = 0x200000;
    REQUIRE(ResolvePlayerIdentity(actor, state) == state);
    REQUIRE(ResolvePlayerIdentity(actor, 0) == actor);

    REQUIRE(!ResolveDownedState(false, 100.0f, 80.0f, 100.0f));
    REQUIRE(ResolveDownedState(false, 0.0f, 80.0f, 100.0f));
    REQUIRE(!ResolveDownedState(true, 100.0f, 0.0f, 0.0f));
    REQUIRE(ResolveDownedState(true, 0.0f, 0.0f, 0.0f));
    REQUIRE(!ResolveDownedState(false, 0.0f, 0.0f, 100.0f));
}
