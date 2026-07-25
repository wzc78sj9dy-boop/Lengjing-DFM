#pragma once

#include "game/native/CoordinatePoolRemotePlan.h"
#include "t3/t3sdk.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lengjing::game::native {

struct CoordinatePoolRemoteRequestKey {
    std::uint64_t mappingBase = 0;
    std::uint64_t mappingSize = 0;
    std::uint64_t rawEntry = 0;
    std::uint64_t codeFingerprint = 0;

    constexpr bool operator==(
        const CoordinatePoolRemoteRequestKey& other) const noexcept {
        return mappingBase == other.mappingBase &&
            mappingSize == other.mappingSize &&
            rawEntry == other.rawEntry &&
            codeFingerprint == other.codeFingerprint;
    }

    constexpr bool operator!=(
        const CoordinatePoolRemoteRequestKey& other) const noexcept {
        return !(*this == other);
    }
};

std::uint64_t CoordinatePoolRemoteCodeFingerprint(
    const std::uint8_t* bytes,
    std::size_t size) noexcept;

struct CoordinatePoolRemoteRequest {
    CoordinatePoolRemoteRequestKey key{};
    std::string deviceId;
    std::vector<std::uint8_t> mapping;
};

enum class CoordinatePoolRemoteSubmitStatus : std::uint8_t {
    Queued,
    Duplicate,
    Cached,
    InvalidRequest,
    Stopped,
};

enum class CoordinatePoolRemoteResultStatus : std::uint8_t {
    Success,
    TransportFailed,
    PlanRejected,
};

struct CoordinatePoolRemoteResult {
    CoordinatePoolRemoteRequestKey key{};
    CoordinatePoolRemoteResultStatus status =
        CoordinatePoolRemoteResultStatus::TransportFailed;
    std::size_t attempt = 0;
    std::chrono::milliseconds nextRetryDelay{0};
    bool retryScheduled = false;
    std::string detail;
    std::shared_ptr<const std::vector<std::uint8_t>> mapping;
    coordinate_pool_internal::coord_dec::RuntimePlan plan;

    bool Ok() const noexcept {
        return status == CoordinatePoolRemoteResultStatus::Success;
    }
};

struct CoordinatePoolRemoteClientOptions {
    std::string endpoint;
    T3HttpTransportOptions transport{};
    std::chrono::milliseconds initialRetryDelay{500};
    std::chrono::milliseconds maximumRetryDelay{8000};
    std::size_t maximumTransportAttempts = 3;
    std::size_t maximumMappingBytes = 2U * 1024U * 1024U;
    std::size_t maximumCachedResults = 4;

    bool IsValid() const noexcept;
};

struct CoordinatePoolRemoteClientSnapshot {
    CoordinatePoolRemoteRequestKey activeKey{};
    std::uint64_t requestCount = 0;
    std::uint64_t successCount = 0;
    std::uint64_t failureCount = 0;
    std::size_t cachedResultCount = 0;
    bool ready = false;
    bool requestActive = false;
    bool requestPending = false;
    bool stopped = false;
};

class CoordinatePoolRemoteClient final {
public:
    explicit CoordinatePoolRemoteClient(
        CoordinatePoolRemoteClientOptions options,
        std::shared_ptr<T3HttpTransport> transport = {});
    ~CoordinatePoolRemoteClient();

    CoordinatePoolRemoteClient(const CoordinatePoolRemoteClient&) = delete;
    CoordinatePoolRemoteClient& operator=(
        const CoordinatePoolRemoteClient&) = delete;
    CoordinatePoolRemoteClient(CoordinatePoolRemoteClient&&) = delete;
    CoordinatePoolRemoteClient& operator=(
        CoordinatePoolRemoteClient&&) = delete;

    bool IsReady() const noexcept;
    CoordinatePoolRemoteSubmitStatus Submit(
        CoordinatePoolRemoteRequest request);
    bool TryTakeResult(CoordinatePoolRemoteResult& result);
    bool TryGetCached(
        const CoordinatePoolRemoteRequestKey& key,
        CoordinatePoolRemoteResult& result);
    CoordinatePoolRemoteClientSnapshot GetSnapshot() const noexcept;
    void Reset() noexcept;
    void Cancel() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lengjing::game::native
