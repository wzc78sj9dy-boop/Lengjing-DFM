#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace lengjing::auth {

inline constexpr std::uint32_t kCloudLayoutSchemaVersion = 1;

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

struct CloudOffsetLayout {
    std::uintptr_t namePoolOffset = 0;
    std::uintptr_t worldOffset = 0;
    std::array<std::uintptr_t, 2> geometryInstancePointerOffsets{};
    CloudActorRecordLayout actorRecords{};
    std::uintptr_t trackingMatrixRootOffset = 0;
    std::uintptr_t componentPositionFlagOffset = 0;
};

struct CloudLayoutDocument {
    std::uint32_t schemaVersion = 0;
    std::uint64_t revision = 0;
    CloudRuntimeIdentity identity;
    CloudOffsetLayout layout;
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
    explicit CloudLayoutStore(CloudRuntimeIdentity expectedIdentity);

    CloudLayoutStore(const CloudLayoutStore&) = delete;
    CloudLayoutStore& operator=(const CloudLayoutStore&) = delete;

    const CloudRuntimeIdentity& ExpectedIdentity() const noexcept;
    std::shared_ptr<const CloudLayoutDocument> Snapshot() const noexcept;
    CloudLayoutUpdateResult ValidateAndPublish(std::string_view payload);

private:
    CloudRuntimeIdentity expectedIdentity_;
    mutable std::mutex publishMutex_;
    std::shared_ptr<const CloudLayoutDocument> current_;
};

}  // namespace lengjing::auth
