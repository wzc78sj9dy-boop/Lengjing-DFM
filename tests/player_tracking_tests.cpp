#include "test_support.h"

#include "game/native/PlayerTrackingPolicy.h"

void RunPlayerTrackingTests() {
    using lengjing::game::native::IsPlayerTrackable;
    using lengjing::game::native::HasUsablePlayerState;
    using lengjing::game::native::PlayerDirectionData;
    using lengjing::game::native::PlayerTrackingData;
    using lengjing::game::native::ResolvePlayerIdentity;
    using lengjing::game::native::ResolvePlayerTeam;

    const PlayerTrackingData complete{true, true, true};
    REQUIRE(IsPlayerTrackable(complete, PlayerDirectionData{false}));
    REQUIRE(IsPlayerTrackable(complete, PlayerDirectionData{true}));

    REQUIRE(!IsPlayerTrackable(
        PlayerTrackingData{false, true, true},
        PlayerDirectionData{true}));
    REQUIRE(!IsPlayerTrackable(
        PlayerTrackingData{true, false, true},
        PlayerDirectionData{true}));
    REQUIRE(!IsPlayerTrackable(
        PlayerTrackingData{true, true, false},
        PlayerDirectionData{true}));

    REQUIRE(HasUsablePlayerState(true, false));
    REQUIRE(HasUsablePlayerState(false, true));
    REQUIRE(!HasUsablePlayerState(false, false));

    REQUIRE(ResolvePlayerTeam(true, 7, false) == 7);
    REQUIRE(ResolvePlayerTeam(false, 7, false) == -1);
    REQUIRE(ResolvePlayerTeam(false, -1, true) == 0);

    constexpr std::uintptr_t actor = 0x100000;
    constexpr std::uintptr_t state = 0x200000;
    REQUIRE(ResolvePlayerIdentity(actor, state) == state);
    REQUIRE(ResolvePlayerIdentity(actor, 0) == actor);
}
