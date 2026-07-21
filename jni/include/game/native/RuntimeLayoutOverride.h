#pragma once

#include "auth/CloudLayout.h"
#include "game/native/ActorRecordResolver.h"
#include "game/native/CoordinatePoolRuntime.h"
#include "game/native/MemoryTransport.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace lengjing::game::native {

namespace detail {

inline constexpr std::uintptr_t kMaximumModuleOffset = 0xffffffffULL;
inline constexpr std::uintptr_t kMaximumObjectOffset = 0xffffULL;

constexpr bool IsOptionalOffsetValid(std::uintptr_t value,
                                     std::uintptr_t minimum,
                                     std::uintptr_t maximum,
                                     std::uintptr_t alignment) noexcept {
    return value == 0 ||
        (value >= minimum && value <= maximum &&
         (alignment == 0 || value % alignment == 0));
}

constexpr bool IsActorLayoutValid(
    const auth::CloudActorRecordLayout& actor) noexcept {
    if (!IsOptionalOffsetValid(
            actor.taggedContainerOffset, 4, kMaximumModuleOffset, 4) ||
        !IsOptionalOffsetValid(
            actor.plainArrayOffset, 4, kMaximumModuleOffset, 4) ||
        !IsOptionalOffsetValid(
            actor.plainRootOffset, 4, kMaximumObjectOffset, 4) ||
        !IsOptionalOffsetValid(
            actor.plainMeshOffset, 4, kMaximumObjectOffset, 4) ||
        actor.encryptedRecordCount > 65536 ||
        actor.plainRecordStride > 256 ||
        actor.maximumPlainCount < 0 ||
        actor.maximumPlainCount > 65536 ||
        actor.fallbackPlainCount < 0 ||
        actor.fallbackPlainCount > 65536) {
        return false;
    }

    const bool taggedEnabled = actor.taggedContainerOffset != 0;
    if (taggedEnabled != (actor.encryptedRecordCount != 0)) return false;

    const bool plainEnabled = actor.plainArrayOffset != 0;
    const bool plainFieldsPresent = actor.plainRootOffset != 0 &&
        actor.plainMeshOffset != 0 && actor.plainRecordStride >= 8 &&
        actor.plainRecordStride % 8 == 0 &&
        actor.maximumPlainCount != 0 && actor.fallbackPlainCount != 0 &&
        actor.fallbackPlainCount <= actor.maximumPlainCount;
    const bool plainFieldsEmpty = actor.plainRootOffset == 0 &&
        actor.plainMeshOffset == 0 && actor.plainRecordStride == 0 &&
        actor.maximumPlainCount == 0 && actor.fallbackPlainCount == 0;
    return plainEnabled ? plainFieldsPresent : plainFieldsEmpty;
}

constexpr bool IsCloudOffsetLayoutValid(
    const auth::CloudOffsetLayout& layout) noexcept {
    if (!IsOptionalOffsetValid(
            layout.namePoolOffset, 4, kMaximumModuleOffset, 4) ||
        !IsOptionalOffsetValid(
            layout.worldOffset, 4, kMaximumModuleOffset, 4) ||
        layout.namePoolOffset == 0 || layout.worldOffset == 0 ||
        layout.namePoolOffset == layout.worldOffset ||
        !IsOptionalOffsetValid(
            layout.coordinateReplayEntryOffset,
            4, kMaximumModuleOffset, 4) ||
        !IsOptionalOffsetValid(
            layout.trackingMatrixRootOffset,
            4, kMaximumModuleOffset, 4) ||
        !IsOptionalOffsetValid(
            layout.componentPositionFlagOffset,
            4, kMaximumModuleOffset, 1) ||
        !IsActorLayoutValid(layout.actorRecords)) {
        return false;
    }
    for (const std::uintptr_t geometryOffset :
         layout.geometryInstancePointerOffsets) {
        if (!IsOptionalOffsetValid(
                geometryOffset, 8, kMaximumModuleOffset, 8)) {
            return false;
        }
    }
    return true;
}

}  // namespace detail

struct RuntimeLayoutOverride {
    std::uintptr_t namePoolOffset = 0;
    std::uintptr_t worldOffset = 0;
    std::uintptr_t coordinateReplayEntryOffset = 0;
    std::array<std::uintptr_t, 2> geometryInstancePointerOffsets{};
    ActorRecordLayout actorRecords{};
    std::uintptr_t trackingMatrixRootOffset = 0;
    std::uintptr_t componentPositionFlagOffset = 0;
    CoordinatePoolRuntimeLayout coordinatePool{};
    CoordinateReplayTransportLayout coordinateTransport{};
};

inline std::optional<RuntimeLayoutOverride> BuildRuntimeLayoutOverride(
    const auth::CloudLayoutDocument* document,
    std::string_view expectedPackage,
    std::string_view expectedModule,
    std::string_view runtimeBuildId) noexcept {
    if (document == nullptr || expectedPackage.empty() ||
        expectedModule.empty() || runtimeBuildId.empty() ||
        document->schemaVersion != auth::kCloudLayoutSchemaVersion ||
        document->revision == 0 || !document->identity.IsValid() ||
        document->identity.packageName != expectedPackage ||
        document->identity.moduleName != expectedModule ||
        document->identity.buildId != runtimeBuildId ||
        !detail::IsCloudOffsetLayoutValid(document->layout) ||
        document->layout.coordinatePool.rootRva ==
            document->layout.namePoolOffset ||
        document->layout.coordinatePool.rootRva ==
            document->layout.worldOffset) {
        return std::nullopt;
    }

    const auth::CloudActorRecordLayout& actor =
        document->layout.actorRecords;
    RuntimeLayoutOverride result{};
    result.namePoolOffset = document->layout.namePoolOffset;
    result.worldOffset = document->layout.worldOffset;
    result.coordinateReplayEntryOffset =
        document->layout.coordinateReplayEntryOffset;
    result.geometryInstancePointerOffsets =
        document->layout.geometryInstancePointerOffsets;
    result.actorRecords = {
        actor.taggedContainerOffset,
        actor.plainArrayOffset,
        actor.plainRootOffset,
        actor.plainMeshOffset,
        actor.encryptedRecordCount,
        actor.plainRecordStride,
        actor.maximumPlainCount,
        actor.fallbackPlainCount,
    };
    result.trackingMatrixRootOffset =
        document->layout.trackingMatrixRootOffset;
    result.componentPositionFlagOffset =
        document->layout.componentPositionFlagOffset;
    const auth::CloudCoordinatePoolLayout& coordinate =
        document->layout.coordinatePool;
    result.coordinatePool = {
        coordinate.rootRva,
        coordinate.bridgeOffset,
        coordinate.contextOffset,
        coordinate.entryOffset,
        coordinate.componentKeyOffset,
        coordinate.entryStride,
        coordinate.poolHeadSkip,
        coordinate.ringRefreshFrames,
    };
    result.coordinateTransport = {
        coordinate.rootRva,
        coordinate.bridgeOffset,
        coordinate.entryOffset,
        coordinate.pacgaData,
        coordinate.pacgaModifier,
    };
    if (!result.coordinatePool.IsValid() ||
        !result.coordinateTransport.IsValid()) {
        return std::nullopt;
    }
    return result;
}

}  // namespace lengjing::game::native
