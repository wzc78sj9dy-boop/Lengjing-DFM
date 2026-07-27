#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace lengjing::auth {

inline constexpr std::uint32_t kCloudLayoutSchemaVersion = 5;
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

struct CloudActorSubjectLayout {
    std::uintptr_t rootOffset = 0;
    std::uintptr_t meshOffset = 0;
    std::uintptr_t alternateRootOffset = 0;
};

struct CloudOffsetLayout {
    std::uintptr_t namePoolOffset = 0;
    std::uintptr_t worldOffset = 0;
    std::array<std::uintptr_t, 2> geometryInstancePointerOffsets{};
    std::int32_t maximumActorCount = 0;
    CloudActorSubjectLayout actorSubject{};
    std::uintptr_t trackingMatrixRootOffset = 0;
};

struct CloudDecryptLayout {
    std::uintptr_t firstVeneerRva = 0;
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
