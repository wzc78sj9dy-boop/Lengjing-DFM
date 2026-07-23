#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace lengjing::game::native {

inline constexpr std::uintptr_t kComponentPositionOffsetWhenFlagSet = 0x148;
inline constexpr std::uintptr_t kComponentPositionOffsetWhenFlagClear = 0x220;
inline constexpr std::uintptr_t kComponentEulerOffset = 0x178;
inline constexpr std::uintptr_t kComponentScaleOffset = 0x184;
inline constexpr float kComponentDegreesToRadians = 0.0174532924f;
inline constexpr float kComponentYawAdjustment = -90.0f;
inline constexpr float kResolvedBoneVerticalAdjustment = -86.0f;

using ComponentPositionFlag = std::uint32_t;

struct ComponentVector3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct ComponentEulerAngles {
    float pitch = 0.0f;
    float yaw = 0.0f;
    float roll = 0.0f;
};

struct ComponentQuaternion {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

struct ResolvedComponentTransform {
    ComponentQuaternion rotation{};
    ComponentVector3 translation{};
    ComponentVector3 scale{};
};

struct ResolvedComponentFieldAddresses {
    std::uintptr_t position = 0;
    std::uintptr_t euler = 0;
    std::uintptr_t scale = 0;
};
static_assert(sizeof(ComponentVector3) == 12);
static_assert(sizeof(ComponentEulerAngles) == 12);
static_assert(sizeof(ComponentQuaternion) == 16);
static_assert(sizeof(ResolvedComponentTransform) == 40);
static_assert(offsetof(ResolvedComponentTransform, rotation) == 0);
static_assert(offsetof(ResolvedComponentTransform, translation) == 16);
static_assert(offsetof(ResolvedComponentTransform, scale) == 28);

constexpr std::uintptr_t SelectComponentPositionOffset(
    ComponentPositionFlag flag) noexcept {
    return flag != 0
        ? kComponentPositionOffsetWhenFlagSet
        : kComponentPositionOffsetWhenFlagClear;
}

constexpr ResolvedComponentFieldAddresses
ResolveComponentFieldAddresses(
    std::uintptr_t root,
    ComponentPositionFlag flag) noexcept {
    return ResolvedComponentFieldAddresses{
        root + SelectComponentPositionOffset(flag),
        root + kComponentEulerOffset,
        root + kComponentScaleOffset,
    };
}

inline void ComponentSinCos(
    float angle,
    float& sine,
    float& cosine) noexcept {
#if defined(__ANDROID__)
    ::sincosf(angle, &sine, &cosine);
#elif defined(__GNUC__) && !defined(__clang__)
    __builtin_sincosf(angle, &sine, &cosine);
#else
    sine = std::sin(angle);
    cosine = std::cos(angle);
#endif
}

inline ComponentQuaternion BuildResolvedComponentQuaternion(
    const ComponentEulerAngles& euler) noexcept {
    const float halfPitch =
        euler.pitch * kComponentDegreesToRadians * 0.5f;
    const float halfYaw =
        (euler.yaw + kComponentYawAdjustment) *
        kComponentDegreesToRadians * 0.5f;
    const float halfRoll =
        euler.roll * kComponentDegreesToRadians * 0.5f;

    float sinPitch = 0.0f;
    float cosPitch = 0.0f;
    float sinYaw = 0.0f;
    float cosYaw = 0.0f;
    float sinRoll = 0.0f;
    float cosRoll = 0.0f;
    ComponentSinCos(halfPitch, sinPitch, cosPitch);
    ComponentSinCos(halfYaw, sinYaw, cosYaw);
    ComponentSinCos(halfRoll, sinRoll, cosRoll);

    const float pitchRoll = sinPitch * sinRoll;
    const float negativeRollCosPitch = -(sinRoll * cosPitch);
    const float cosPitchCosRoll = cosPitch * cosRoll;
    const float sinPitchCosRoll = sinPitch * cosRoll;
    const float negativeCosRollSinPitch = -(cosRoll * sinPitch);

    const float yawPitchRoll = sinYaw * pitchRoll;
    const float negativeYawRollCosPitch =
        sinYaw * negativeRollCosPitch;
    const float negativeCosYawPitchRoll = -(pitchRoll * cosYaw);
    const float negativeCosYawRollCosPitch =
        cosYaw * negativeRollCosPitch;

    return ComponentQuaternion{
        std::fma(
            sinPitchCosRoll,
            sinYaw,
            negativeCosYawRollCosPitch),
        std::fma(
            negativeCosRollSinPitch,
            cosYaw,
            negativeYawRollCosPitch),
        std::fma(
            cosPitchCosRoll,
            sinYaw,
            negativeCosYawPitchRoll),
        std::fma(cosPitchCosRoll, cosYaw, yawPitchRoll),
    };
}

inline ResolvedComponentTransform BuildResolvedComponentTransform(
    const ComponentVector3& position,
    const ComponentEulerAngles& euler,
    const ComponentVector3& scale) noexcept {
    return ResolvedComponentTransform{
        BuildResolvedComponentQuaternion(euler),
        position,
        scale,
    };
}

inline ComponentVector3 TransformResolvedBoneTranslation(
    const ComponentVector3& local,
    const ResolvedComponentTransform& component) noexcept {
    const float scaledX = component.scale.x * local.x;
    const float scaledY = component.scale.y * local.y;
    const float scaledZ = component.scale.z * local.z;
    const ComponentQuaternion& rotation = component.rotation;

    const float crossX =
        std::fma(rotation.y, scaledZ, -(rotation.z * scaledY));
    const float crossY =
        std::fma(rotation.z, scaledX, -(rotation.x * scaledZ));
    const float crossZ =
        std::fma(rotation.x, scaledY, -(rotation.y * scaledX));

    const float crossCrossX =
        std::fma(rotation.y, crossZ, -(rotation.z * crossY));
    const float crossCrossY =
        std::fma(rotation.z, crossX, -(rotation.x * crossZ));
    const float crossCrossZ =
        std::fma(rotation.x, crossY, -(rotation.y * crossX));

    const float doubledW = rotation.w + rotation.w;
    const float rotatedX =
        (scaledX + doubledW * crossX) +
        (crossCrossX + crossCrossX);
    const float rotatedY =
        (crossCrossY + crossCrossY) +
        (scaledY + doubledW * crossY);
    const float rotatedZ =
        (scaledZ + doubledW * crossZ) +
        (crossCrossZ + crossCrossZ);

    return ComponentVector3{
        component.translation.x + rotatedX,
        component.translation.y + rotatedY,
        (component.translation.z + kResolvedBoneVerticalAdjustment) +
            rotatedZ,
    };
}

}  // namespace lengjing::game::native
