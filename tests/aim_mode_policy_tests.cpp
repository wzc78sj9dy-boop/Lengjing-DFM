#include "test_support.h"

#include "game/aim/AimModePolicy.h"

void RunAimModePolicyTests() {
    using lengjing::game::IsProjectileTrackingRequested;
    using lengjing::game::ResolveProjectileTrackingRequest;
    using lengjing::game::kProjectileTrackingCompiled;
    using lengjing::game::aim::IsAimOutputRequested;
    using lengjing::game::aim::ResolveAimModeActivation;
    using lengjing::game::aim::ResolveAimOutputAvailability;

    REQUIRE(!kProjectileTrackingCompiled);
    REQUIRE(!ResolveProjectileTrackingRequest(false, false));
    REQUIRE(!ResolveProjectileTrackingRequest(false, true));
    REQUIRE(!ResolveProjectileTrackingRequest(true, false));
    REQUIRE(ResolveProjectileTrackingRequest(true, true));
    REQUIRE(!IsProjectileTrackingRequested(true));

    REQUIRE(!IsAimOutputRequested(false, false));
    REQUIRE(IsAimOutputRequested(true, false));
    REQUIRE(!IsAimOutputRequested(false, true));
    REQUIRE(IsAimOutputRequested(true, true));

    const auto trackingOnly = ResolveAimModeActivation(
        true, true, false, true);
    REQUIRE(!trackingOnly.selfAim);
    REQUIRE(!trackingOnly.tracking);
    REQUIRE(!trackingOnly.Any());

    const auto selfAimOnly = ResolveAimModeActivation(
        true, true, true, false);
    REQUIRE(selfAimOnly.selfAim);
    REQUIRE(!selfAimOnly.tracking);
    REQUIRE(selfAimOnly.Any());

    const auto both = ResolveAimModeActivation(
        true, true, true, true);
    REQUIRE(both.selfAim);
    REQUIRE(!both.tracking);

    const auto unavailable = ResolveAimModeActivation(
        false, true, true, true);
    REQUIRE(!unavailable.Any());
    const auto blockedWeapon = ResolveAimModeActivation(
        true, false, true, true);
    REQUIRE(!blockedWeapon.Any());

    const auto trackingTargetOnly = ResolveAimOutputAvailability(
        both, false, true);
    REQUIRE(!trackingTargetOnly.selfAim);
    REQUIRE(!trackingTargetOnly.tracking);

    const auto selfAimTargetOnly = ResolveAimOutputAvailability(
        both, true, false);
    REQUIRE(selfAimTargetOnly.selfAim);
    REQUIRE(!selfAimTargetOnly.tracking);

    const auto noTarget = ResolveAimOutputAvailability(
        both, false, false);
    REQUIRE(!noTarget.Any());
}
