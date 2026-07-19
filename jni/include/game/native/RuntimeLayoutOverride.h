#pragma once

#include "auth/CloudLayout.h"
#include "game/native/ActorRecordResolver.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace lengjing::game::native {

struct RuntimeLayoutOverride {
    std::uintptr_t namePoolOffset = 0;
    std::uintptr_t worldOffset = 0;
    std::uintptr_t coordinateReplayEntryOffset = 0;
    std::array<std::uintptr_t, 2> geometryInstancePointerOffsets{};
    ActorRecordLayout actorRecords{};
    std::uintptr_t trackingMatrixRootOffset = 0;
    std::uintptr_t componentPositionFlagOffset = 0;
};

inline std::optional<RuntimeLayoutOverride> BuildRuntimeLayoutOverride(
    const auth::CloudLayoutDocument* document,
    std::string_view expectedPackage,
    std::string_view expectedModule) noexcept {
    if (document == nullptr || expectedPackage.empty() ||
        expectedModule.empty() ||
        document->schemaVersion != auth::kCloudLayoutSchemaVersion ||
        document->revision == 0 || !document->identity.IsValid() ||
        document->identity.packageName != expectedPackage ||
        document->identity.moduleName != expectedModule ||
        document->layout.namePoolOffset == 0 ||
        document->layout.worldOffset == 0 ||
        document->layout.namePoolOffset == document->layout.worldOffset) {
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
    return result;
}

}  // namespace lengjing::game::native
