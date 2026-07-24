#pragma once

#include "game/GameRuntime.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class T3HttpTransport;

namespace lengjing::diagnostics {

inline constexpr char kDefaultCoordinateFailureUploadUrl[] =
    "http://111.170.14.120:3333/api/v1/lengjing/coordinate-failures";

struct CoordinateFailureMetadata {
    std::string clientVersion;
    std::string deviceHash;
    std::string manufacturer;
    std::string model;
    std::string androidRelease;
    std::string androidSdk;
    std::string abi;
};

CoordinateFailureMetadata CollectCoordinateFailureMetadata(
    std::string_view clientVersion,
    std::string_view deviceSeed);

std::string BuildCoordinateFailureFingerprint(
    const game::RuntimeStatus& status,
    int gameVersionIndex,
    int driverIndex);

std::string BuildCoordinateFailurePayload(
    const game::RuntimeStatus& status,
    const CoordinateFailureMetadata& metadata,
    int gameVersionIndex,
    int driverIndex);

struct CoordinateFailureReportPolicyOptions {
    std::chrono::milliseconds persistenceWindow{
        std::chrono::seconds(5)};
    std::chrono::milliseconds minimumReportInterval{
        std::chrono::minutes(1)};
    std::chrono::milliseconds duplicateWindow{
        std::chrono::minutes(30)};
    std::size_t maximumRememberedFingerprints = 16;
};

class CoordinateFailureReportPolicy final {
public:
    explicit CoordinateFailureReportPolicy(
        CoordinateFailureReportPolicyOptions options = {});

    bool Observe(
        const game::RuntimeStatus& status,
        std::string_view fingerprint,
        std::chrono::steady_clock::time_point now);
    void Reset() noexcept;

private:
    struct ReportedFingerprint {
        std::string value;
        std::chrono::steady_clock::time_point reportedAt{};
    };

    CoordinateFailureReportPolicyOptions options_;
    std::string candidateFingerprint_;
    std::chrono::steady_clock::time_point candidateSince_{};
    std::chrono::steady_clock::time_point lastReportAt_{};
    std::vector<ReportedFingerprint> reportedFingerprints_;
    std::uint64_t observedSuccesses_ = 0;
    bool successCounterInitialized_ = false;
};

struct CoordinateFailureUploaderOptions {
    std::string endpoint = kDefaultCoordinateFailureUploadUrl;
    std::string pendingPath;
    std::vector<std::chrono::milliseconds> retryDelays{
        std::chrono::milliseconds(0),
        std::chrono::seconds(2),
        std::chrono::seconds(10),
    };
    std::size_t maximumPayloadBytes = 16U * 1024U;
    std::size_t maximumQueuedReports = 4;
    CoordinateFailureReportPolicyOptions reportPolicy{};
};

class CoordinateFailureUploader final {
public:
    CoordinateFailureUploader(
        std::shared_ptr<T3HttpTransport> transport,
        CoordinateFailureMetadata metadata,
        CoordinateFailureUploaderOptions options = {});
    ~CoordinateFailureUploader();

    CoordinateFailureUploader(const CoordinateFailureUploader&) = delete;
    CoordinateFailureUploader& operator=(
        const CoordinateFailureUploader&) = delete;

    void Observe(
        const game::RuntimeStatus& status,
        int gameVersionIndex,
        int driverIndex);
    void ResetObservation() noexcept;
    bool Submit(std::string fingerprint, std::string payload);
    bool WaitUntilIdle(std::chrono::milliseconds timeout);
    void Stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lengjing::diagnostics
