#pragma once

#include "auth/CloudLayout.h"
#include "game/native/ActorRecordResolver.h"
#include "game/native/ActorRecordSource.h"
#include "game/native/CoordinateExecutionLayout.h"
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
    return (taggedEnabled || plainEnabled) &&
        (plainEnabled ? plainFieldsPresent : plainFieldsEmpty);
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
        !IsOptionalOffsetValid(
            layout.componentPositionFlagOffset,
            4, kMaximumModuleOffset, 1) ||
        layout.componentPositionFlagOffset == 0 ||
        !IsActorLayoutValid(layout.actorRecords)) {
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

constexpr CoordinatePoolRuntimeLayout MakeCoordinatePoolLayout(
    const auth::CloudCoordinatePoolLayout& pool) noexcept {
    return {
        pool.rootRva,
        pool.bridgeOffset,
        pool.contextOffset,
        pool.entryOffset,
        pool.componentKeyOffset,
        pool.entryStride,
        pool.poolHeadSkip,
        pool.ringRefreshFrames,
    };
}

constexpr CoordinateExecutionLayout MakeCoordinateExecutionLayout(
    const auth::CloudExecutionLayout& execution) noexcept {
    const auth::CloudExecutionHookOffsetLayout& hooks =
        execution.hookOffsets;
    const auth::CloudExecutionFieldOffsetLayout& fields =
        execution.fieldOffsets;
    return {
        {
            execution.discovery.rootOffset,
            execution.discovery.pointerOffset,
            execution.discovery.entryOffset,
            execution.discovery.returnStubMagic,
        },
        {
            execution.result.slotOffset,
            execution.result.positionOffset,
        },
        {
            hooks.subjectLoad,
            hooks.callbackEntry,
            hooks.callbackReturn,
            hooks.callbackIndex,
            hooks.callbackCopyPrepare,
            hooks.callbackCopyAfter,
            hooks.tablePointer,
            hooks.tableValue,
            hooks.lock,
            hooks.lockReturn,
            hooks.firstCall,
            hooks.firstReturn,
            hooks.externalCall,
            hooks.externalReturn,
            hooks.primaryGateWrite,
            hooks.alternateGateWrite,
            hooks.gateProbe,
            hooks.recordCount,
            hooks.targetKey,
            hooks.ringSetup,
            hooks.ringProbe,
            hooks.ringHit,
            hooks.dispatch,
            hooks.dispatchReturn,
            hooks.resultPrepare,
            hooks.result,
        },
        {
            fields.contextExpected,
            fields.stackPriorGate,
            fields.stackPrimaryGateSource,
            fields.stackGateFlag,
            fields.stackGateSnapshotA,
            fields.stackGateSnapshotB,
            fields.stackRingMid,
            fields.objectPosition,
            fields.stackCaptureA,
            fields.stackCaptureB,
            fields.stackCaptureC,
            fields.stackCaptureD,
            fields.captureField,
            fields.stackPoolSelector,
            fields.contextPoolTable,
        },
    };
}

}  // namespace detail

struct RuntimeLayoutOverride {
    std::uintptr_t namePoolOffset = 0;
    std::uintptr_t worldOffset = 0;
    std::array<std::uintptr_t, 2> geometryInstancePointerOffsets{};
    ActorRecordLayout actorRecords{};
    ActorSubjectLayout actorSubject{};
    std::uintptr_t trackingMatrixRootOffset = 0;
    std::uintptr_t componentPositionFlagOffset = 0;
    CoordinatePoolRuntimeLayout coordinatePool{};
    CoordinatePoolRuntimeLayout coordinateDecrypt2Pool{};
    CoordinateReplayTransportLayout coordinateTransport{};
    CoordinateExecutionLayout coordinateExecution{};
    CoordinateExecutionContextLayout coordinateExecutionContext{};
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

    const auth::CloudActorRecordLayout& actor =
        document->layout.actorRecords;
    const auth::CloudActorSubjectLayout& subject =
        document->layout.actorSubject;
    const auth::CloudDecryptMode1Layout& mode1 = document->decrypt.mode1;
    const auth::CloudDecryptMode2Layout& mode2 = document->decrypt.mode2;
    RuntimeLayoutOverride result{};
    result.namePoolOffset = document->layout.namePoolOffset;
    result.worldOffset = document->layout.worldOffset;
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
    result.actorSubject = {
        subject.rootOffset,
        subject.meshOffset,
        subject.alternateRootOffset,
    };
    result.trackingMatrixRootOffset =
        document->layout.trackingMatrixRootOffset;
    result.componentPositionFlagOffset =
        document->layout.componentPositionFlagOffset;
    result.coordinatePool = detail::MakeCoordinatePoolLayout(mode1.pool);
    result.coordinateDecrypt2Pool =
        detail::MakeCoordinatePoolLayout(mode2.pool);
    result.coordinateTransport = {
        mode1.pool.rootRva,
        mode1.pool.bridgeOffset,
        mode1.pool.entryOffset,
        mode1.pacgaData,
        mode1.pacgaModifier,
    };
    result.coordinateExecution =
        detail::MakeCoordinateExecutionLayout(document->decrypt.execution);
    result.coordinateExecutionContext = {
        document->decrypt.execution.context.threadName,
        document->decrypt.execution.context.oracleOpcode,
    };

    const bool rootsConflict =
        result.coordinatePool.rootRva == result.namePoolOffset ||
        result.coordinatePool.rootRva == result.worldOffset ||
        result.coordinateDecrypt2Pool.rootRva == result.namePoolOffset ||
        result.coordinateDecrypt2Pool.rootRva == result.worldOffset;
    if (rootsConflict || !result.actorSubject.IsValid() ||
        !result.coordinatePool.IsValid() ||
        !result.coordinateDecrypt2Pool.IsValid() ||
        !result.coordinateTransport.IsValid() ||
        !result.coordinateExecution.IsValid() ||
        !result.coordinateExecutionContext.IsValid()) {
        return std::nullopt;
    }
    return result;
}

}  // namespace lengjing::game::native
