#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace lengjing::auth {

inline constexpr std::uint32_t kCloudLayoutSchemaVersion = 4;
inline constexpr std::size_t kMaximumCloudLayoutPayloadBytes = 1000U;

struct CloudRuntimeTarget {
    std::string packageName;
    std::string moduleName;

    bool IsValid() const noexcept;
};

struct CloudRuntimeIdentity {
    std::string packageName;
    std::string moduleName;
    std::string buildId;

    bool IsValid() const noexcept;
};

struct CloudActorRecordLayout {
    std::uintptr_t taggedContainerOffset = 0;
    std::uintptr_t plainArrayOffset = 0;
    std::uintptr_t plainRootOffset = 0;
    std::uintptr_t plainMeshOffset = 0;
    std::uint32_t encryptedRecordCount = 0;
    std::uint32_t plainRecordStride = 0;
    std::int32_t maximumPlainCount = 0;
    std::int32_t fallbackPlainCount = 0;
};

struct CloudActorSubjectLayout {
    std::uintptr_t rootOffset = 0;
    std::uintptr_t meshOffset = 0;
    std::uintptr_t alternateRootOffset = 0;
};

struct CloudCoordinatePoolLayout {
    std::uintptr_t rootRva = 0;
    std::uintptr_t bridgeOffset = 0;
    std::int32_t contextOffset = 0;
    std::uintptr_t entryOffset = 0;
    std::uintptr_t componentKeyOffset = 0;
    std::uint32_t entryStride = 0;
    std::uint32_t poolHeadSkip = 0;
    std::uint32_t ringRefreshFrames = 0;
};

struct CloudOffsetLayout {
    std::uintptr_t namePoolOffset = 0;
    std::uintptr_t worldOffset = 0;
    std::array<std::uintptr_t, 2> geometryInstancePointerOffsets{};
    CloudActorRecordLayout actorRecords{};
    CloudActorSubjectLayout actorSubject{};
    std::uintptr_t trackingMatrixRootOffset = 0;
    std::uintptr_t componentPositionFlagOffset = 0;
};

struct CloudDecryptMode1Layout {
    CloudCoordinatePoolLayout pool{};
    std::uint64_t pacgaData = 0;
    std::uint64_t pacgaModifier = 0;
};

struct CloudDecryptMode2Layout {
    CloudCoordinatePoolLayout pool{};
};

struct CloudExecutionDiscoveryLayout {
    std::uintptr_t rootOffset = 0;
    std::uintptr_t pointerOffset = 0;
    std::uintptr_t entryOffset = 0;
    std::uint64_t returnStubMagic = 0;
};

struct CloudExecutionResultLayout {
    std::uintptr_t slotOffset = 0;
    std::uintptr_t positionOffset = 0;
};

struct CloudExecutionHookOffsetLayout {
    std::uintptr_t subjectLoad = 0;
    std::uintptr_t callbackEntry = 0;
    std::uintptr_t callbackReturn = 0;
    std::uintptr_t callbackIndex = 0;
    std::uintptr_t callbackCopyPrepare = 0;
    std::uintptr_t callbackCopyAfter = 0;
    std::uintptr_t tablePointer = 0;
    std::uintptr_t tableValue = 0;
    std::uintptr_t lock = 0;
    std::uintptr_t lockReturn = 0;
    std::uintptr_t firstCall = 0;
    std::uintptr_t firstReturn = 0;
    std::uintptr_t externalCall = 0;
    std::uintptr_t externalReturn = 0;
    std::uintptr_t primaryGateWrite = 0;
    std::uintptr_t alternateGateWrite = 0;
    std::uintptr_t gateProbe = 0;
    std::uintptr_t recordCount = 0;
    std::uintptr_t targetKey = 0;
    std::uintptr_t ringSetup = 0;
    std::uintptr_t ringProbe = 0;
    std::uintptr_t ringHit = 0;
    std::uintptr_t dispatch = 0;
    std::uintptr_t dispatchReturn = 0;
    std::uintptr_t resultPrepare = 0;
    std::uintptr_t result = 0;
};

struct CloudExecutionFieldOffsetLayout {
    std::uintptr_t contextExpected = 0;
    std::uintptr_t stackPriorGate = 0;
    std::uintptr_t stackPrimaryGateSource = 0;
    std::uintptr_t stackGateFlag = 0;
    std::uintptr_t stackGateSnapshotA = 0;
    std::uintptr_t stackGateSnapshotB = 0;
    std::uintptr_t stackRingMid = 0;
    std::uintptr_t objectPosition = 0;
    std::uintptr_t stackCaptureA = 0;
    std::uintptr_t stackCaptureB = 0;
    std::uintptr_t stackCaptureC = 0;
    std::uintptr_t stackCaptureD = 0;
    std::uintptr_t captureField = 0;
    std::uintptr_t stackPoolSelector = 0;
    std::uintptr_t contextPoolTable = 0;
};

struct CloudExecutionContextLayout {
    std::string threadName;
    std::uint32_t oracleOpcode = 0;
};

struct CloudExecutionLayout {
    CloudExecutionDiscoveryLayout discovery{};
    CloudExecutionResultLayout result{};
    CloudExecutionHookOffsetLayout hookOffsets{};
    CloudExecutionFieldOffsetLayout fieldOffsets{};
    CloudExecutionContextLayout context{};
};

struct CloudDecryptLayout {
    CloudDecryptMode1Layout mode1{};
    CloudDecryptMode2Layout mode2{};
    CloudExecutionLayout execution{};
};

struct CloudLayoutDocument {
    std::uint32_t schemaVersion = 0;
    std::uint64_t revision = 0;
    CloudRuntimeIdentity identity{};
    CloudOffsetLayout layout{};
    CloudDecryptLayout decrypt{};
};

enum class CloudLayoutStatus {
    Published,
    Unchanged,
    NotConfigured,
    SessionInvalid,
    FetchFailed,
    InvalidJson,
    SchemaMismatch,
    IdentityMismatch,
    RangeError,
    RollbackRejected,
    RevisionConflict,
};

struct CloudLayoutUpdateResult {
    CloudLayoutStatus status = CloudLayoutStatus::InvalidJson;
    std::string detail;
    std::shared_ptr<const CloudLayoutDocument> snapshot;

    bool Succeeded() const noexcept {
        return status == CloudLayoutStatus::Published ||
            status == CloudLayoutStatus::Unchanged;
    }
};

class CloudLayoutStore final {
public:
    explicit CloudLayoutStore(CloudRuntimeTarget target);

    CloudLayoutStore(const CloudLayoutStore&) = delete;
    CloudLayoutStore& operator=(const CloudLayoutStore&) = delete;

    const CloudRuntimeTarget& ExpectedTarget() const noexcept;
    std::shared_ptr<const CloudLayoutDocument> Snapshot() const noexcept;
    CloudLayoutUpdateResult ValidateAndPublish(std::string_view payload);

private:
    CloudRuntimeTarget target_;
    mutable std::mutex publishMutex_;
    std::shared_ptr<const CloudLayoutDocument> current_;
};

}  // namespace lengjing::auth
