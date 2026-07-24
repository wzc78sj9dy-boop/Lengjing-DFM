#include "game/native/CharacterComponentTransform.h"
#include "test_support.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

bool NearlyEqual(float left, float right, float tolerance = 0.00001f) {
    return std::fabs(left - right) <= tolerance;
}

}  // namespace

void RunCharacterComponentTransformTests() {
    using namespace lengjing::game::native;

    REQUIRE(SelectComponentPositionOffset(0) == 0x220);
    REQUIRE(SelectComponentPositionOffset(1) == 0x148);
    REQUIRE(SelectComponentPositionOffset(0xFFFFFFFFU) == 0x148);
    REQUIRE(sizeof(ComponentPositionFlag) == 4);
    REQUIRE(kComponentEulerOffset == 0x178);
    REQUIRE(kComponentScaleOffset == 0x184);
    const ResolvedComponentFieldAddresses clearAddresses =
        ResolveComponentFieldAddresses(0x1000, 0);
    REQUIRE(
        clearAddresses.position ==
        0x1000 + kComponentPositionOffsetWhenFlagClear);
    REQUIRE(clearAddresses.euler == 0x1000 + kComponentEulerOffset);
    REQUIRE(clearAddresses.scale == 0x1000 + kComponentScaleOffset);
    const ResolvedComponentFieldAddresses setAddresses =
        ResolveComponentFieldAddresses(0x1000, 1);
    REQUIRE(
        setAddresses.position ==
        0x1000 + kComponentPositionOffsetWhenFlagSet);

    std::uint32_t degreesToRadiansBits = 0;
    std::memcpy(
        &degreesToRadiansBits,
        &kComponentDegreesToRadians,
        sizeof(degreesToRadiansBits));
    REQUIRE(degreesToRadiansBits == 0x3C8EFA35U);

    const ComponentQuaternion identity =
        BuildResolvedComponentQuaternion({0.0f, 90.0f, 0.0f});
    REQUIRE(NearlyEqual(identity.x, 0.0f));
    REQUIRE(NearlyEqual(identity.y, 0.0f));
    REQUIRE(NearlyEqual(identity.z, 0.0f));
    REQUIRE(NearlyEqual(identity.w, 1.0f));

    const ComponentQuaternion zeroEuler =
        BuildResolvedComponentQuaternion({0.0f, 0.0f, 0.0f});
    REQUIRE(NearlyEqual(zeroEuler.x, 0.0f));
    REQUIRE(NearlyEqual(zeroEuler.y, 0.0f));
    REQUIRE(NearlyEqual(zeroEuler.z, -0.707106769f));
    REQUIRE(NearlyEqual(zeroEuler.w, 0.707106769f));

    const ComponentQuaternion fullRotation =
        BuildResolvedComponentQuaternion({10.0f, 20.0f, 30.0f});
    REQUIRE(NearlyEqual(fullRotation.x, -0.2594925f));
    REQUIRE(NearlyEqual(fullRotation.y, 0.07892648f));
    REQUIRE(NearlyEqual(fullRotation.z, -0.5704021f));
    REQUIRE(NearlyEqual(fullRotation.w, 0.7752907f));

    const ResolvedComponentTransform scaledIdentity =
        BuildResolvedComponentTransform(
            {100.0f, 200.0f, 300.0f},
            {0.0f, 90.0f, 0.0f},
            {2.0f, 3.0f, 4.0f});
    REQUIRE(NearlyEqual(scaledIdentity.rotation.w, identity.w));
    REQUIRE(NearlyEqual(scaledIdentity.translation.x, 100.0f));
    REQUIRE(NearlyEqual(scaledIdentity.scale.z, 4.0f));
    const ComponentVector3 scaledWorld =
        TransformResolvedBoneTranslation(
            {1.0f, 2.0f, 3.0f}, scaledIdentity);
    REQUIRE(NearlyEqual(scaledWorld.x, 102.0f));
    REQUIRE(NearlyEqual(scaledWorld.y, 206.0f));
    REQUIRE(NearlyEqual(scaledWorld.z, 226.0f));

    const ComponentVector3 decodedAlignment =
        BuildResolvedBoneAlignment(
            {1000.0f, 2000.0f, 3000.0f},
            scaledIdentity.translation);
    REQUIRE(NearlyEqual(decodedAlignment.x, 900.0f));
    REQUIRE(NearlyEqual(decodedAlignment.y, 1800.0f));
    REQUIRE(NearlyEqual(decodedAlignment.z, 2786.0f));
    REQUIRE(NearlyEqual(
        scaledIdentity.translation.z +
            kResolvedBoneVerticalAdjustment +
            decodedAlignment.z,
        3000.0f));

    const ResolvedComponentTransform yawRotation{
        BuildResolvedComponentQuaternion({0.0f, 180.0f, 0.0f}),
        {},
        {1.0f, 1.0f, 1.0f},
    };
    const ComponentVector3 yawWorld =
        TransformResolvedBoneTranslation({1.0f, 0.0f, 0.0f}, yawRotation);
    REQUIRE(NearlyEqual(yawWorld.x, 0.0f));
    REQUIRE(NearlyEqual(yawWorld.y, 1.0f));
    REQUIRE(NearlyEqual(yawWorld.z, -86.0f));

    const ResolvedComponentTransform translatedYaw{
        zeroEuler,
        {10.0f, 20.0f, 100.0f},
        {1.0f, 1.0f, 1.0f},
    };
    const ComponentVector3 translatedYawWorld =
        TransformResolvedBoneTranslation(
            {1.0f, 2.0f, 3.0f}, translatedYaw);
    REQUIRE(NearlyEqual(translatedYawWorld.x, 12.0f));
    REQUIRE(NearlyEqual(translatedYawWorld.y, 19.0f));
    REQUIRE(NearlyEqual(translatedYawWorld.z, 17.0f));

    const ResolvedComponentTransform mixedTransform =
        BuildResolvedComponentTransform(
            {100.0f, 200.0f, 300.0f},
            {10.0f, 20.0f, 30.0f},
            {2.0f, 3.0f, 4.0f});
    const ComponentVector3 mixedWorld =
        TransformResolvedBoneTranslation(
            {1.0f, 2.0f, 3.0f}, mixedTransform);
    REQUIRE(NearlyEqual(mixedWorld.x, 110.755554f));
    REQUIRE(NearlyEqual(mixedWorld.y, 203.184723f));
    REQUIRE(NearlyEqual(mixedWorld.z, 221.627289f));

    const ResolvedComponentTransform pitchRotation{
        BuildResolvedComponentQuaternion({90.0f, 90.0f, 0.0f}),
        {},
        {3.0f, 1.0f, 1.0f},
    };
    const ComponentVector3 pitchWorld =
        TransformResolvedBoneTranslation({2.0f, 0.0f, 0.0f}, pitchRotation);
    REQUIRE(NearlyEqual(pitchWorld.x, 0.0f));
    REQUIRE(NearlyEqual(pitchWorld.y, 0.0f));
    REQUIRE(NearlyEqual(pitchWorld.z, -80.0f));
}
