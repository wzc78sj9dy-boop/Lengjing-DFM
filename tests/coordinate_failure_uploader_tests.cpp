#include "test_support.h"

#include "diagnostics/CoordinateFailureUploader.h"
#include "t3/t3sdk.h"
#include "vendor/json.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace {

class FakeDiagnosticTransport final : public T3HttpTransport {
public:
    explicit FakeDiagnosticTransport(bool succeed)
        : succeed_(succeed) {}

    T3HttpTransportResult post(
        const std::string& url,
        const std::string& contentType,
        const std::string& body) override {
        if (cancelled_.load(std::memory_order_acquire)) {
            return {false, {}, "cancelled"};
        }
        {
            std::lock_guard lock(mutex_);
            ++requests_;
            url_ = url;
            contentType_ = contentType;
            body_ = body;
        }
        return succeed_
            ? T3HttpTransportResult{true, "ok", {}}
            : T3HttpTransportResult{false, {}, "offline"};
    }

    void cancelPendingRequests() noexcept override {
        cancelled_.store(true, std::memory_order_release);
    }

    void resetCancellation() noexcept override {
        cancelled_.store(false, std::memory_order_release);
    }

    int Requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }

    std::string Url() const {
        std::lock_guard lock(mutex_);
        return url_;
    }

    std::string ContentType() const {
        std::lock_guard lock(mutex_);
        return contentType_;
    }

    std::string Body() const {
        std::lock_guard lock(mutex_);
        return body_;
    }

private:
    bool succeed_ = false;
    std::atomic_bool cancelled_{false};
    mutable std::mutex mutex_;
    int requests_ = 0;
    std::string url_;
    std::string contentType_;
    std::string body_;
};

lengjing::game::RuntimeStatus FailedStatus() {
    using namespace lengjing::game;
    RuntimeStatus status;
    status.phase = RuntimePhase::Running;
    status.baseReady = true;
    status.coordinateRequested = true;
    status.coordinateEntryReady = true;
    status.coordinateContextReady = true;
    status.coordinateAttempts = 7;
    status.coordinateError = CoordinateDecryptError::CodeAnalysisFailed;
    status.coordinateRead.stage = CoordinateReadStage::CodePage;
    status.coordinateRead.failure = CoordinateReadFailure::ShortRead;
    status.coordinateRead.address = UINT64_C(0x123456789abcdef0);
    status.coordinateRead.size = 4096;
    status.coordinateRead.attemptCount = 2;
    status.coordinateRead.systemError = -5;
    status.coordinatePoolPointer.address =
        UINT64_C(0x1111222233334444);
    status.coordinatePoolPointer.rawValue =
        UINT64_C(0x5555666677778888);
    status.coordinateEntry.entry = UINT64_C(0x9999aaaabbbbcccc);
    status.coordinateEntry.mappingStart =
        UINT64_C(0x1000000010000000);
    status.coordinateEntry.mappingEnd =
        UINT64_C(0x1000000010200000);
    status.coordinateEntry.mappingFragments = 2;
    status.coordinateRuntimeDetail.executionSource = 2;
    status.coordinateRuntimeDetail.ptraceSystemError = 0;
    status.coordinateRuntimeDetail.pacgaOracleAvailable = true;
    status.coordinateRuntimeDetail.poolStage = 5;
    status.coordinateRuntimeDetail.poolError = 10;
    status.coordinateRuntimeDetail.analysisFailure = 4;
    status.coordinateRuntimeDetail.analysisFindStage = 2;
    status.coordinateRuntimeDetail.analysisFindDetail = 9;
    status.coordinateRuntimeDetail.analysisMaddCount = 6;
    status.coordinateRuntimeDetail.analysisRingMaddCount = 3;
    status.coordinateRuntimeDetail.analysisCandidateCount = 2;
    status.coordinateRuntimeDetail.analysisFailureInstruction = 411;
    status.coordinateRuntimeDetail.analysisMode = 1;
    status.coordinateRuntimeDetail.analysisPasses = 3;
    status.coordinateRuntimeDetail.analysisLoadedPages = 17;
    status.coordinateRuntimeDetail.analysisRequestedMethods = 4;
    status.coordinateRuntimeDetail.analysisSkippedPages = 1;
    status.coordinateRuntimeDetail.logicalSlotCount = 8;
    status.coordinateRuntimeDetail.physicalSlotCount = 12;
    return status;
}

}  // namespace

void RunCoordinateFailureUploaderTests() {
    using namespace std::chrono;
    using namespace lengjing::diagnostics;

    const auto status = FailedStatus();
    CoordinateFailureMetadata metadata;
    metadata.clientVersion = "1.0.0";
    metadata.deviceHash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    metadata.manufacturer = "vendor";
    metadata.model = "model";
    metadata.androidRelease = "15";
    metadata.androidSdk = "35";
    metadata.abi = "arm64-v8a";

    const std::string fingerprint =
        BuildCoordinateFailureFingerprint(status, 0, 2);
    REQUIRE(fingerprint.size() == 16);
    const std::string payload =
        BuildCoordinateFailurePayload(status, metadata, 0, 2);
    const nlohmann::json report = nlohmann::json::parse(payload);
    REQUIRE(report.at("schema") == 1);
    REQUIRE(report.at("type") == "coordinate_decrypt_1_failure");
    REQUIRE(report.at("fingerprint") == fingerprint);
    REQUIRE(report.at("coordinate").at("error") == 1005);
    REQUIRE(
        report.at("coordinate").at("pool").at("analysis_failure") == 4);
    REQUIRE(
        report.at("coordinate").at("pool").at("analysis_find_detail") == 9);
    REQUIRE(
        report.at("coordinate").at("pool").at("analysis_madd_count") == 6);
    REQUIRE(
        report.at("coordinate").at("pool").at(
            "analysis_ring_madd_count") == 3);
    REQUIRE(
        report.at("coordinate").at("pool").at(
            "analysis_candidate_count") == 2);
    REQUIRE(
        report.at("coordinate").at("pool").at(
            "analysis_failure_instruction") == 411);
    REQUIRE(
        report.at("coordinate").at("pool").at("analysis_loaded_pages") ==
        17);
    REQUIRE(payload.find("\"address\"") == std::string::npos);
    REQUIRE(payload.find("123456789abcdef0") == std::string::npos);
    REQUIRE(payload.find("1111222233334444") == std::string::npos);
    REQUIRE(payload.find("5555666677778888") == std::string::npos);
    REQUIRE(CollectCoordinateFailureMetadata("1.0.0", {}).deviceHash.size() ==
            32);

    CoordinateFailureReportPolicyOptions policyOptions;
    policyOptions.persistenceWindow = milliseconds(100);
    policyOptions.minimumReportInterval = milliseconds(200);
    policyOptions.duplicateWindow = seconds(1);
    CoordinateFailureReportPolicy policy(policyOptions);
    const auto start = steady_clock::now();
    REQUIRE(!policy.Observe(status, fingerprint, start));
    REQUIRE(!policy.Observe(status, fingerprint, start + milliseconds(1)));
    REQUIRE(policy.Observe(
        status, fingerprint, start + milliseconds(101)));
    REQUIRE(!policy.Observe(
        status, fingerprint, start + milliseconds(400)));
    REQUIRE(policy.Observe(
        status, fingerprint, start + milliseconds(1101)));

    auto recovering = status;
    policy.Reset();
    REQUIRE(!policy.Observe(recovering, fingerprint, start));
    recovering.coordinateSuccesses = 1;
    REQUIRE(!policy.Observe(
        recovering, fingerprint, start + milliseconds(150)));
    recovering.coordinateSuccesses = 2;
    REQUIRE(!policy.Observe(
        recovering, fingerprint, start + milliseconds(300)));

    const auto unique = duration_cast<nanoseconds>(
        system_clock::now().time_since_epoch()).count();
    const std::filesystem::path pendingPath =
        std::filesystem::temp_directory_path() /
        ("lengjing_coordinate_upload_" + std::to_string(unique) +
         ".pending");
    std::remove(pendingPath.string().c_str());

    CoordinateFailureUploaderOptions uploaderOptions;
    uploaderOptions.pendingPath = pendingPath.string();
    uploaderOptions.retryDelays = {milliseconds(0)};
    uploaderOptions.maximumPayloadBytes = 16U * 1024U;

    {
        auto transport =
            std::make_shared<FakeDiagnosticTransport>(true);
        CoordinateFailureUploader uploader(
            transport, metadata, uploaderOptions);
        REQUIRE(uploader.Submit(fingerprint, payload));
        REQUIRE(uploader.WaitUntilIdle(seconds(1)));
        REQUIRE(transport->Requests() == 1);
        REQUIRE(transport->Url() ==
                kDefaultCoordinateFailureUploadUrl);
        REQUIRE(transport->ContentType() == "application/json");
        REQUIRE(transport->Body() == payload);
        REQUIRE(!std::filesystem::exists(pendingPath));
        REQUIRE(uploader.Submit(fingerprint, payload));
        REQUIRE(uploader.WaitUntilIdle(seconds(1)));
        REQUIRE(transport->Requests() == 2);
    }

    {
        auto transport =
            std::make_shared<FakeDiagnosticTransport>(false);
        CoordinateFailureUploader uploader(
            transport, metadata, uploaderOptions);
        REQUIRE(uploader.Submit("deadbeefdeadbeef", payload));
        REQUIRE(uploader.WaitUntilIdle(seconds(1)));
        REQUIRE(std::filesystem::exists(pendingPath));
    }
    {
        auto transport =
            std::make_shared<FakeDiagnosticTransport>(true);
        CoordinateFailureUploader uploader(
            transport, metadata, uploaderOptions);
        REQUIRE(uploader.WaitUntilIdle(seconds(1)));
        REQUIRE(transport->Requests() == 1);
        REQUIRE(!std::filesystem::exists(pendingPath));
    }

    std::remove(pendingPath.string().c_str());
}
