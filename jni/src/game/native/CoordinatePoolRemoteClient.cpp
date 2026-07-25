#include "game/native/CoordinatePoolRemoteClient.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

namespace lengjing::game::native {
namespace {

constexpr std::size_t kMaximumEndpointBytes = 2048;
constexpr std::size_t kMaximumDeviceIdBytes = 256;
constexpr std::size_t kMaximumMappingLimit = 64U * 1024U * 1024U;
constexpr std::size_t kMaximumCacheEntries = 32;
constexpr std::size_t kMaximumCompletedResults = 16;
constexpr std::size_t kMaximumTransportAttempts = 16;
constexpr auto kMaximumBackoff = std::chrono::minutes(2);

bool HasHttpScheme(std::string_view value) noexcept {
    return value.compare(0, 7, "http://") == 0 ||
        value.compare(0, 8, "https://") == 0;
}

bool IsPrintableField(std::string_view value) noexcept {
    return !value.empty() &&
        std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return character >= 0x21U && character <= 0x7eU;
        });
}

bool RequestIsValid(
    const CoordinatePoolRemoteRequest& request,
    std::size_t maximumMappingBytes) noexcept {
    if (request.key.mappingBase == 0 ||
        (request.key.mappingBase & 3U) != 0 ||
        request.mapping.size() < sizeof(std::uint32_t) ||
        request.mapping.size() > maximumMappingBytes ||
        request.key.mappingSize != request.mapping.size() ||
        request.mapping.size() >
            std::numeric_limits<std::uint64_t>::max() -
                request.key.mappingBase ||
        request.deviceId.size() > kMaximumDeviceIdBytes ||
        !IsPrintableField(request.deviceId)) {
        return false;
    }
    const std::uint64_t mappingEnd =
        request.key.mappingBase + request.mapping.size();
    if ((request.key.rawEntry & 3U) != 0 ||
        request.key.rawEntry < request.key.mappingBase ||
        request.key.rawEntry > mappingEnd - sizeof(std::uint32_t)) {
        return false;
    }
    return request.key.codeFingerprint != 0 &&
        request.key.codeFingerprint == CoordinatePoolRemoteCodeFingerprint(
            request.mapping.data(), request.mapping.size());
}

std::string Hex(std::uint64_t value) {
    char buffer[17]{};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%016llx",
        static_cast<unsigned long long>(value));
    return buffer;
}

bool Contains(
    const std::vector<std::uint8_t>& bytes,
    std::string_view needle) noexcept {
    if (needle.empty() || bytes.size() < needle.size()) return false;
    return std::search(
               bytes.begin(),
               bytes.end(),
               needle.begin(),
               needle.end()) != bytes.end();
}

std::string SelectBoundary(const CoordinatePoolRemoteRequest& request) {
    const std::string prefix = "----lengjing-" +
        Hex(request.key.codeFingerprint) + '-';
    for (std::uint32_t salt = 0;; ++salt) {
        const std::string boundary = prefix + std::to_string(salt);
        if (!Contains(request.mapping, boundary) &&
            request.deviceId.find(boundary) == std::string::npos) {
            return boundary;
        }
    }
}

void AppendTextPart(std::string& body,
                    std::string_view boundary,
                    std::string_view name,
                    std::string_view value) {
    body.append("--");
    body.append(boundary);
    body.append("\r\nContent-Disposition: form-data; name=\"");
    body.append(name);
    body.append("\"\r\n\r\n");
    body.append(value);
    body.append("\r\n");
}

struct MultipartRequest {
    std::string contentType;
    std::string body;
};

MultipartRequest BuildMultipartRequest(
    const CoordinatePoolRemoteRequest& request) {
    MultipartRequest output;
    const std::string boundary = SelectBoundary(request);
    output.contentType = "multipart/form-data; boundary=" + boundary;
    output.body.reserve(request.mapping.size() + 1024U);
    output.body.append("--");
    output.body.append(boundary);
    output.body.append(
        "\r\nContent-Disposition: form-data; name=\"file\"; "
        "filename=\"dump.bin\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n");
    output.body.append(
        reinterpret_cast<const char*>(request.mapping.data()),
        request.mapping.size());
    output.body.append("\r\n");
    AppendTextPart(
        output.body,
        boundary,
        "entry",
        std::to_string(request.key.rawEntry));
    AppendTextPart(
        output.body,
        boundary,
        "base",
        std::to_string(request.key.mappingBase));
    AppendTextPart(output.body, boundary, "id", request.deviceId);
    output.body.append("--");
    output.body.append(boundary);
    output.body.append("--\r\n");
    return output;
}

std::chrono::milliseconds NextBackoff(
    std::chrono::milliseconds current,
    std::chrono::milliseconds maximum) noexcept {
    if (current >= maximum) return maximum;
    if (current.count() > maximum.count() / 2) return maximum;
    return std::min(maximum, current * 2);
}

}  // namespace

std::uint64_t CoordinatePoolRemoteCodeFingerprint(
    const std::uint8_t* bytes,
    std::size_t size) noexcept {
    constexpr std::uint64_t kOffsetBasis =
        UINT64_C(14695981039346656037);
    constexpr std::uint64_t kPrime = UINT64_C(1099511628211);
    std::uint64_t fingerprint = kOffsetBasis;
    if (bytes == nullptr) return size == 0 ? fingerprint : 0;
    for (std::size_t index = 0; index < size; ++index) {
        fingerprint ^= bytes[index];
        fingerprint *= kPrime;
    }
    return fingerprint;
}

bool CoordinatePoolRemoteClientOptions::IsValid() const noexcept {
    if (endpoint.empty() || endpoint.size() > kMaximumEndpointBytes ||
        !HasHttpScheme(endpoint) || !transport.isValid() ||
        initialRetryDelay.count() <= 0 ||
        maximumRetryDelay < initialRetryDelay ||
        maximumRetryDelay > kMaximumBackoff ||
        maximumTransportAttempts == 0 ||
        maximumTransportAttempts > kMaximumTransportAttempts ||
        maximumMappingBytes < sizeof(std::uint32_t) ||
        maximumMappingBytes > kMaximumMappingLimit ||
        maximumCachedResults == 0 ||
        maximumCachedResults > kMaximumCacheEntries) {
        return false;
    }
    return std::all_of(
        endpoint.begin(), endpoint.end(), [](unsigned char character) {
            return character >= 0x21U && character <= 0x7eU;
        });
}

struct CoordinatePoolRemoteClient::Impl final {
    explicit Impl(CoordinatePoolRemoteClientOptions configuredOptions,
                  std::shared_ptr<T3HttpTransport> configuredTransport)
        : options(std::move(configuredOptions)),
          transport(std::move(configuredTransport)) {
        if (!options.IsValid()) return;
        if (transport == nullptr) {
            transport = createT3DefaultHttpTransport(options.transport);
        }
        if (transport == nullptr) return;
        ready = true;
        worker = std::thread([this] { WorkerEntry(); });
    }

    ~Impl() { Cancel(); }

    void PushCompleted(CoordinatePoolRemoteResult result) {
        if (completed.size() == kMaximumCompletedResults) {
            completed.pop_front();
        }
        completed.push_back(std::move(result));
    }

    void CacheSuccess(const CoordinatePoolRemoteResult& result) {
        const auto existing = std::find_if(
            cache.begin(), cache.end(), [&](const auto& candidate) {
                return candidate.key == result.key;
            });
        if (existing != cache.end()) cache.erase(existing);
        cache.insert(cache.begin(), result);
        if (cache.size() > options.maximumCachedResults) cache.pop_back();
    }

    void WorkerMain() {
        for (;;) {
            CoordinatePoolRemoteRequest request;
            std::uint64_t requestGeneration = 0;
            {
                std::unique_lock<std::mutex> lock(mutex);
                readyCondition.wait(lock, [&] {
                    return stopping || pending.has_value();
                });
                if (stopping) break;
                request = std::move(*pending);
                pending.reset();
                activeKey = request.key;
                requestGeneration = generation;
            }

            transport->resetCancellation();
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (stopping) return;
                if (requestGeneration != generation) {
                    activeKey.reset();
                    continue;
                }
            }
            const MultipartRequest multipart = BuildMultipartRequest(request);
            std::size_t attempt = 0;
            std::chrono::milliseconds retryDelay =
                options.initialRetryDelay;
            bool requestComplete = false;
            while (!requestComplete) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stopping) return;
                    if (requestGeneration != generation) {
                        activeKey.reset();
                        requestComplete = true;
                        continue;
                    }
                    ++requestCount;
                }

                ++attempt;
                T3HttpTransportResult transportResult = transport->post(
                    options.endpoint,
                    multipart.contentType,
                    multipart.body);

                CoordinatePoolRemoteResult completedResult;
                completedResult.key = request.key;
                completedResult.attempt = attempt;
                if (!transportResult.success) {
                    completedResult.status =
                        CoordinatePoolRemoteResultStatus::TransportFailed;
                    completedResult.detail = transportResult.error.empty()
                        ? "remote plan transport failed"
                        : std::move(transportResult.error);
                    completedResult.retryScheduled =
                        attempt < options.maximumTransportAttempts;
                    if (completedResult.retryScheduled) {
                        completedResult.nextRetryDelay = retryDelay;
                    }
                } else {
                    CoordinatePoolRemotePlanResult parsed =
                        ParseCoordinatePoolRemotePlan(
                            transportResult.body,
                            request.key.mappingBase,
                            request.mapping.size(),
                            request.key.rawEntry);
                    if (parsed.Ok()) {
                        completedResult.status =
                            CoordinatePoolRemoteResultStatus::Success;
                        completedResult.mapping =
                            std::make_shared<const std::vector<std::uint8_t>>(
                                std::move(request.mapping));
                        completedResult.plan = std::move(parsed.plan);
                    } else {
                        completedResult.status =
                            CoordinatePoolRemoteResultStatus::PlanRejected;
                        completedResult.detail = parsed.detail.empty()
                            ? "remote plan was rejected"
                            : std::move(parsed.detail);
                    }
                }

                std::unique_lock<std::mutex> lock(mutex);
                if (stopping) return;
                if (requestGeneration != generation) {
                    activeKey.reset();
                    requestComplete = true;
                    continue;
                }
                if (completedResult.Ok()) {
                    ++successCount;
                    CacheSuccess(completedResult);
                    PushCompleted(std::move(completedResult));
                    activeKey.reset();
                    requestComplete = true;
                    continue;
                }
                ++failureCount;
                const bool retryScheduled =
                    completedResult.retryScheduled;
                PushCompleted(std::move(completedResult));
                if (!retryScheduled) {
                    activeKey.reset();
                    requestComplete = true;
                    continue;
                }
                if (pending.has_value()) {
                    activeKey.reset();
                    requestComplete = true;
                    continue;
                }
                readyCondition.wait_for(lock, retryDelay, [&] {
                    return stopping || pending.has_value() ||
                        requestGeneration != generation;
                });
                if (stopping) return;
                if (requestGeneration != generation) {
                    activeKey.reset();
                    requestComplete = true;
                    continue;
                }
                if (pending.has_value()) {
                    activeKey.reset();
                    requestComplete = true;
                    continue;
                }
                retryDelay = NextBackoff(
                    retryDelay, options.maximumRetryDelay);
            }
        }
    }

    void WorkerEntry() noexcept {
        try {
            WorkerMain();
        } catch (const std::exception& exception) {
            StopAfterWorkerException(exception.what());
        } catch (...) {
            StopAfterWorkerException("remote plan worker failed");
        }
    }

    void StopAfterWorkerException(const char* detail) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex);
            ready = false;
            stopping = true;
            pending.reset();
            ++failureCount;
            if (activeKey.has_value()) {
                CoordinatePoolRemoteResult result;
                result.key = *activeKey;
                result.status =
                    CoordinatePoolRemoteResultStatus::PlanRejected;
                result.detail = detail == nullptr
                    ? "remote plan worker failed"
                    : detail;
                PushCompleted(std::move(result));
            }
            activeKey.reset();
            readyCondition.notify_all();
        } catch (...) {
        }
    }

    CoordinatePoolRemoteSubmitStatus Submit(
        CoordinatePoolRemoteRequest request) {
        if (!RequestIsValid(request, options.maximumMappingBytes)) {
            return CoordinatePoolRemoteSubmitStatus::InvalidRequest;
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (!ready || stopping) {
            return CoordinatePoolRemoteSubmitStatus::Stopped;
        }
        const auto cached = std::find_if(
            cache.begin(), cache.end(), [&](const auto& candidate) {
                return candidate.key == request.key;
            });
        if (cached != cache.end()) {
            return CoordinatePoolRemoteSubmitStatus::Cached;
        }
        if ((activeKey.has_value() && *activeKey == request.key) ||
            (pending.has_value() && pending->key == request.key)) {
            return CoordinatePoolRemoteSubmitStatus::Duplicate;
        }
        pending = std::move(request);
        readyCondition.notify_one();
        return CoordinatePoolRemoteSubmitStatus::Queued;
    }

    bool TryTakeResult(CoordinatePoolRemoteResult& result) {
        std::lock_guard<std::mutex> lock(mutex);
        if (completed.empty()) return false;
        result = std::move(completed.front());
        completed.pop_front();
        return true;
    }

    bool TryGetCached(const CoordinatePoolRemoteRequestKey& key,
                      CoordinatePoolRemoteResult& result) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto cached = std::find_if(
            cache.begin(), cache.end(), [&](const auto& candidate) {
                return candidate.key == key;
            });
        if (cached == cache.end()) return false;
        result = *cached;
        if (cached != cache.begin()) {
            CoordinatePoolRemoteResult value = *cached;
            cache.erase(cached);
            cache.insert(cache.begin(), std::move(value));
        }
        return true;
    }

    CoordinatePoolRemoteClientSnapshot GetSnapshot() const noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        CoordinatePoolRemoteClientSnapshot snapshot;
        if (activeKey.has_value()) snapshot.activeKey = *activeKey;
        snapshot.requestCount = requestCount;
        snapshot.successCount = successCount;
        snapshot.failureCount = failureCount;
        snapshot.cachedResultCount = cache.size();
        snapshot.ready = ready && !stopping;
        snapshot.requestActive = activeKey.has_value();
        snapshot.requestPending = pending.has_value();
        snapshot.stopped = stopping;
        return snapshot;
    }

    void Reset() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) return;
            ++generation;
            pending.reset();
            activeKey.reset();
            completed.clear();
            cache.clear();
            readyCondition.notify_all();
        }
        if (transport != nullptr) transport->cancelPendingRequests();
    }

    void Cancel() noexcept {
        std::thread joinedWorker;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping && !worker.joinable()) return;
            stopping = true;
            pending.reset();
            readyCondition.notify_all();
            if (worker.joinable()) joinedWorker = std::move(worker);
        }
        if (transport != nullptr) transport->cancelPendingRequests();
        if (joinedWorker.joinable()) joinedWorker.join();
        std::lock_guard<std::mutex> lock(mutex);
        ready = false;
        activeKey.reset();
    }

    CoordinatePoolRemoteClientOptions options;
    std::shared_ptr<T3HttpTransport> transport;
    mutable std::mutex mutex;
    std::condition_variable readyCondition;
    std::optional<CoordinatePoolRemoteRequest> pending;
    std::optional<CoordinatePoolRemoteRequestKey> activeKey;
    std::deque<CoordinatePoolRemoteResult> completed;
    std::vector<CoordinatePoolRemoteResult> cache;
    std::thread worker;
    bool ready = false;
    bool stopping = false;
    std::uint64_t requestCount = 0;
    std::uint64_t successCount = 0;
    std::uint64_t failureCount = 0;
    std::uint64_t generation = 0;
};

CoordinatePoolRemoteClient::CoordinatePoolRemoteClient(
    CoordinatePoolRemoteClientOptions options,
    std::shared_ptr<T3HttpTransport> transport)
    : impl_(std::make_unique<Impl>(
          std::move(options), std::move(transport))) {}

CoordinatePoolRemoteClient::~CoordinatePoolRemoteClient() = default;

bool CoordinatePoolRemoteClient::IsReady() const noexcept {
    if (impl_ == nullptr) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->ready && !impl_->stopping;
}

CoordinatePoolRemoteSubmitStatus CoordinatePoolRemoteClient::Submit(
    CoordinatePoolRemoteRequest request) {
    return impl_ == nullptr
        ? CoordinatePoolRemoteSubmitStatus::Stopped
        : impl_->Submit(std::move(request));
}

bool CoordinatePoolRemoteClient::TryTakeResult(
    CoordinatePoolRemoteResult& result) {
    return impl_ != nullptr && impl_->TryTakeResult(result);
}

bool CoordinatePoolRemoteClient::TryGetCached(
    const CoordinatePoolRemoteRequestKey& key,
    CoordinatePoolRemoteResult& result) {
    return impl_ != nullptr && impl_->TryGetCached(key, result);
}

CoordinatePoolRemoteClientSnapshot
CoordinatePoolRemoteClient::GetSnapshot() const noexcept {
    return impl_ == nullptr
        ? CoordinatePoolRemoteClientSnapshot{}
        : impl_->GetSnapshot();
}

void CoordinatePoolRemoteClient::Reset() noexcept {
    if (impl_ != nullptr) impl_->Reset();
}

void CoordinatePoolRemoteClient::Cancel() noexcept {
    if (impl_ != nullptr) impl_->Cancel();
}

}  // namespace lengjing::game::native
