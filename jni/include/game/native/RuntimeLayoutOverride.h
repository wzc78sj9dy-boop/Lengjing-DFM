#pragma once

#include "auth/CloudLayout.h"
#include "game/native/ActorRecordSource.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace lengjing::game::native {

namespace detail {

inline constexpr std::uintptr_t kMaximumModuleOffset = 0xffffffffULL;

constexpr bool IsOptionalOffsetValid(std::uintptr_t value,
                                     std::uintptr_t minimum,
                                     std::uintptr_t maximum,
                                     std::uintptr_t alignment) noexcept {
    return value == 0 ||
        (value >= minimum && value <= maximum &&
         (alignment == 0 || value % alignment == 0));
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
            layout.trackingMatrixRootOffset,
            4, kMaximumModuleOffset, 4) ||
        layout.trackingMatrixRootOffset == 0 ||
        layout.maximumActorCount < 1 ||
        layout.maximumActorCount > 65536) {
        return false;
    }
    for (const std::uintptr_t geometryOffset :
         layout.geometryInstancePointerOffsets) {
        if (!IsOptionalOffsetValid(
                geometryOffset, 8, kMaximumModuleOffset, 8) ||
            geometryOffset == 0) {
            return false;
        }
    }
    return layout.geometryInstancePointerOffsets[0] !=
        layout.geometryInstancePointerOffsets[1];
}

}  // namespace detail

struct RuntimeLayoutOverride {
    std::uintptr_t namePoolOffset = 0;
    std::uintptr_t worldOffset = 0;
    std::array<std::uintptr_t, 2> geometryInstancePointerOffsets{};
    std::int32_t maximumActorCount = 0;
    ActorSubjectLayout actorSubject{};
    std::uintptr_t trackingMatrixRootOffset = 0;
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
        !detail::IsCloudOffsetLayoutValid(document->layout)) {
        return std::nullopt;
    }

    const auth::CloudActorSubjectLayout& subject =
        document->layout.actorSubject;
    RuntimeLayoutOverride result{};
    result.namePoolOffset = document->layout.namePoolOffset;
    result.worldOffset = document->layout.worldOffset;
    result.geometryInstancePointerOffsets =
        document->layout.geometryInstancePointerOffsets;
    result.maximumActorCount = document->layout.maximumActorCount;
    result.actorSubject = {
        subject.rootOffset,
        subject.meshOffset,
        subject.alternateRootOffset,
    };
    result.trackingMatrixRootOffset =
        document->layout.trackingMatrixRootOffset;

    if (!result.actorSubject.IsValid()) {
        return std::nullopt;
    }
    return result;
}

}  // namespace lengjing::game::native
