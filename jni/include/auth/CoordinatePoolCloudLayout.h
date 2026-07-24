#pragma once

#include "auth/CloudLayout.h"
#include "game/native/CoordinatePoolRuntime.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace lengjing::auth {

inline constexpr std::uint32_t kCoordinatePoolCloudLayoutSchemaVersion = 1;
inline constexpr std::size_t kMaximumCoordinatePoolCloudLayoutPayloadBytes =
    64U * 1024U;

struct CoordinatePoolCloudLayoutDocument {
    std::uint32_t schemaVersion = 0;
    std::uint64_t revision = 0;
    CloudRuntimeIdentity identity;
    game::native::CoordinatePoolRuntimeLayout coordinatePool;
};

struct CoordinatePoolCloudLayoutUpdateResult {
    CloudLayoutStatus status = CloudLayoutStatus::InvalidJson;
    std::string detail;
    std::shared_ptr<const CoordinatePoolCloudLayoutDocument> snapshot;

    bool Succeeded() const noexcept {
        return status == CloudLayoutStatus::Published ||
            status == CloudLayoutStatus::Unchanged;
    }
};

class CoordinatePoolCloudLayoutStore final {
public:
    explicit CoordinatePoolCloudLayoutStore(
        CloudRuntimeIdentity expectedIdentity);

    CoordinatePoolCloudLayoutStore(
        const CoordinatePoolCloudLayoutStore&) = delete;
    CoordinatePoolCloudLayoutStore& operator=(
        const CoordinatePoolCloudLayoutStore&) = delete;

    const CloudRuntimeIdentity& ExpectedIdentity() const noexcept;
    std::shared_ptr<const CoordinatePoolCloudLayoutDocument>
    Snapshot() const noexcept;
    CoordinatePoolCloudLayoutUpdateResult ValidateAndPublish(
        std::string_view payload);

private:
    CloudRuntimeIdentity expectedIdentity_;
    mutable std::mutex publishMutex_;
    std::shared_ptr<const CoordinatePoolCloudLayoutDocument> current_;
};

}  // namespace lengjing::auth
