#include "diagnostics/CoordinateFailureUploader.h"

#include "t3/t3sdk.h"
#include "vendor/json.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#if defined(__ANDROID__)
#include <sys/stat.h>
#include <sys/system_properties.h>
#endif

namespace lengjing::diagnostics {
namespace {

constexpr std::string_view kPendingMagic = "LJCF1";
constexpr std::size_t kMaximumMetadataBytes = 96;

std::string SanitizeMetadata(std::string value) {
    value.erase(
        std::remove_if(
            value.begin(), value.end(), [](unsigned char character) {
                return character < 0x20U || character == 0x7fU;
            }),
        value.end());
    if (value.size() > kMaximumMetadataBytes) {
        value.resize(kMaximumMetadataBytes);
    }
    return value;
}

std::string HashIdentifier(std::string_view value) {
    constexpr std::uint64_t kFnvPrime = UINT64_C(1099511628211);
    std::uint64_t first = UINT64_C(1469598103934665603);
    std::uint64_t second = UINT64_C(1099511628211);
    constexpr std::string_view kDomain = "lengjing-coordinate-report|";
    const auto update = [=](std::uint64_t& hash, unsigned char byte) {
        hash = (hash ^ byte) * kFnvPrime;
    };
    for (const unsigned char byte : kDomain) {
        update(first, byte);
        update(second, static_cast<unsigned char>(byte + 0x39U));
    }
    for (const unsigned char byte : value) {
        update(first, byte);
        update(second, static_cast<unsigned char>(byte + 0x5dU));
    }
    std::ostringstream stream;
    stream << std::hex << std::nouppercase << std::setfill('0')
           << std::setw(16) << first << std::setw(16) << second;
    return stream.str();
}

std::string HashFingerprint(std::string_view value) {
    constexpr std::uint64_t kFnvPrime = UINT64_C(1099511628211);
    std::uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char byte : value) {
        hash = (hash ^ byte) * kFnvPrime;
    }
    std::ostringstream stream;
    stream << std::hex << std::nouppercase << std::setfill('0')
           << std::setw(16) << hash;
    return stream.str();
}

#if defined(__ANDROID__)
std::string ReadSystemProperty(const char* name) {
    char buffer[PROP_VALUE_MAX]{};
    const int length = __system_property_get(name, buffer);
    if (length <= 0) return {};
    return SanitizeMetadata(
        std::string(buffer, static_cast<std::size_t>(length)));
}
#endif

bool IsFailureActive(const game::RuntimeStatus& status) noexcept {
    return status.coordinateRequested &&
        status.coordinateError != game::CoordinateDecryptError::None &&
        status.phase != game::RuntimePhase::Stopped &&
        status.phase != game::RuntimePhase::Stopping;
}

bool IsValidEndpoint(std::string_view endpoint) noexcept {
    if (endpoint.empty() || endpoint.size() > 2048U) return false;
    if (endpoint.compare(0, 7, "http://") != 0 &&
        endpoint.compare(0, 8, "https://") != 0) {
        return false;
    }
    return std::none_of(
        endpoint.begin(), endpoint.end(), [](unsigned char character) {
            return character <= 0x20U || character == 0x7fU;
        });
}

bool IsValidFingerprint(std::string_view fingerprint) noexcept {
    return fingerprint.size() == 16U &&
        std::all_of(
            fingerprint.begin(), fingerprint.end(),
            [](unsigned char character) {
                return (character >= '0' && character <= '9') ||
                    (character >= 'a' && character <= 'f');
            });
}

std::string SanitizeUploadError(std::string value) {
    std::replace(value.begin(), value.end(), '\r', ' ');
    std::replace(value.begin(), value.end(), '\n', ' ');
    if (value.size() > 160U) value.resize(160U);
    return value;
}

}  // namespace

CoordinateFailureMetadata CollectCoordinateFailureMetadata(
    std::string_view clientVersion,
    std::string_view deviceSeed) {
    CoordinateFailureMetadata metadata;
    metadata.clientVersion =
        SanitizeMetadata(std::string(clientVersion));
    metadata.deviceHash = HashIdentifier(deviceSeed);
#if defined(__ANDROID__)
    metadata.manufacturer = ReadSystemProperty("ro.product.manufacturer");
    metadata.model = ReadSystemProperty("ro.product.model");
    metadata.androidRelease =
        ReadSystemProperty("ro.build.version.release");
    metadata.androidSdk = ReadSystemProperty("ro.build.version.sdk");
    metadata.abi = ReadSystemProperty("ro.product.cpu.abi");
#endif
    return metadata;
}

std::string BuildCoordinateFailureFingerprint(
    const game::RuntimeStatus& status,
    int gameVersionIndex,
    int driverIndex) {
    const game::CoordinateRuntimeDetail& detail =
        status.coordinateRuntimeDetail;
    std::ostringstream stream;
    stream
        << gameVersionIndex << '|' << driverIndex << '|'
        << game::CoordinateDecryptErrorCode(status.coordinateError) << '|'
        << status.coordinateSystemError << '|'
        << static_cast<unsigned>(status.coordinateRead.stage) << '|'
        << static_cast<unsigned>(status.coordinateRead.failure) << '|'
        << status.coordinateRead.systemError << '|'
        << game::CoordinateDecryptErrorCode(
               status.coordinatePoolPointer.error) << '|'
        << status.coordinatePoolPointer.systemError << '|'
        << status.coordinateEntry.mappingFragments << '|'
        << static_cast<unsigned>(detail.executionSource) << '|'
        << detail.executionError << '|'
        << detail.executionSystemError << '|'
        << detail.deviceSystemError << '|'
        << detail.ptraceSystemError << '|'
        << static_cast<unsigned>(detail.poolStage) << '|'
        << static_cast<unsigned>(detail.poolError) << '|'
        << detail.poolSystemError << '|'
        << static_cast<unsigned>(detail.analysisFailure) << '|'
        << static_cast<unsigned>(detail.analysisFindStage) << '|'
        << static_cast<unsigned>(detail.analysisFindDetail) << '|'
        << detail.analysisMaddCount << '|'
        << detail.analysisRingMaddCount << '|'
        << detail.analysisCandidateCount << '|'
        << detail.analysisFailureInstruction << '|'
        << static_cast<unsigned>(detail.analysisMode) << '|'
        << detail.analysisSkippedPages << '|'
        << static_cast<unsigned>(detail.analysisSkippedFailure) << '|'
        << detail.analysisSkippedSystemError << '|'
        << static_cast<unsigned>(detail.logicalSlotCount) << '|'
        << static_cast<unsigned>(detail.physicalSlotCount) << '|'
        << static_cast<unsigned>(detail.slotLayoutKind);
    return HashFingerprint(stream.str());
}

std::string BuildCoordinateFailurePayload(
    const game::RuntimeStatus& status,
    const CoordinateFailureMetadata& metadata,
    int gameVersionIndex,
    int driverIndex) {
    using nlohmann::json;
    const game::CoordinateReadDiagnostic& read = status.coordinateRead;
    const game::CoordinatePoolPointerDiagnostic& poolPointer =
        status.coordinatePoolPointer;
    const game::CoordinateRuntimeDetail& detail =
        status.coordinateRuntimeDetail;

    const auto now = std::chrono::system_clock::now();
    const auto timestamp = std::chrono::duration_cast<
        std::chrono::milliseconds>(now.time_since_epoch()).count();

    json report{
        {"schema", 1},
        {"type", "coordinate_decrypt_1_failure"},
        {"fingerprint",
         BuildCoordinateFailureFingerprint(
             status, gameVersionIndex, driverIndex)},
        {"client_time_ms", timestamp},
        {"client",
         {
             {"version", metadata.clientVersion},
             {"device_hash", metadata.deviceHash},
             {"manufacturer", metadata.manufacturer},
             {"model", metadata.model},
             {"android_release", metadata.androidRelease},
             {"android_sdk", metadata.androidSdk},
             {"abi", metadata.abi},
         }},
        {"selection",
         {
             {"game_version", gameVersionIndex},
             {"driver", driverIndex},
         }},
        {"runtime",
         {
             {"phase", static_cast<unsigned>(status.phase)},
             {"base_ready", status.baseReady},
         }},
        {"coordinate",
         {
             {"requested", status.coordinateRequested},
             {"entry_ready", status.coordinateEntryReady},
             {"context_ready", status.coordinateContextReady},
             {"context_generation", status.coordinateContextGeneration},
             {"attempts", status.coordinateAttempts},
             {"successes", status.coordinateSuccesses},
             {"error",
              game::CoordinateDecryptErrorCode(status.coordinateError)},
             {"system_error", status.coordinateSystemError},
             {"read",
              {
                  {"stage", static_cast<unsigned>(read.stage)},
                  {"primary_path",
                   static_cast<unsigned>(read.primaryPath)},
                  {"last_path", static_cast<unsigned>(read.lastPath)},
                  {"failure", static_cast<unsigned>(read.failure)},
                  {"attempted_paths", read.attemptedPaths},
                  {"attempt_count", read.attemptCount},
                  {"size", read.size},
                  {"primary_completed", read.primaryCompleted},
                  {"last_completed", read.lastCompleted},
                  {"batch_items", read.batchItemCount},
                  {"batch_failed_index", read.batchFailedIndex},
                  {"failed_item_completed", read.failedItemCompleted},
                  {"primary_system_error", read.primarySystemError},
                  {"last_system_error", read.lastSystemError},
                  {"system_error", read.systemError},
              }},
             {"pool_pointer",
              {
                  {"error",
                   game::CoordinateDecryptErrorCode(poolPointer.error)},
                  {"state_flags", poolPointer.stateFlags},
                  {"offset", poolPointer.offset},
                  {"system_error", poolPointer.systemError},
              }},
             {"entry",
              {
                  {"mapping_fragments",
                   status.coordinateEntry.mappingFragments},
              }},
             {"execution",
              {
                  {"source", detail.executionSource},
                  {"error", detail.executionError},
                  {"system_error", detail.executionSystemError},
                  {"device_system_error", detail.deviceSystemError},
                  {"ptrace_system_error", detail.ptraceSystemError},
                  {"device_requests", detail.deviceRequestCount},
                  {"operands_resolved", detail.pacgaOperandsResolved},
                  {"key_available", detail.pacgaKeyAvailable},
                  {"oracle_available", detail.pacgaOracleAvailable},
              }},
             {"pool",
              {
                  {"ready", detail.poolReady},
                  {"stage", detail.poolStage},
                  {"error", detail.poolError},
                  {"system_error", detail.poolSystemError},
                  {"analysis_failure", detail.analysisFailure},
                  {"analysis_find_stage", detail.analysisFindStage},
                  {"analysis_find_detail", detail.analysisFindDetail},
                  {"analysis_madd_count", detail.analysisMaddCount},
                  {"analysis_ring_madd_count",
                   detail.analysisRingMaddCount},
                  {"analysis_candidate_count",
                   detail.analysisCandidateCount},
                  {"analysis_failure_instruction",
                   detail.analysisFailureInstruction},
                  {"analysis_mode", detail.analysisMode},
                  {"analysis_passes", detail.analysisPasses},
                  {"analysis_loaded_pages", detail.analysisLoadedPages},
                  {"analysis_requested_methods",
                   detail.analysisRequestedMethods},
                  {"analysis_skipped_pages",
                   detail.analysisSkippedPages},
                  {"analysis_skipped_failure",
                   detail.analysisSkippedFailure},
                  {"analysis_skipped_system_error",
                   detail.analysisSkippedSystemError},
                  {"decoded_slot_mask", detail.decodedSlotMask},
                  {"compact_phase_mask", detail.compactPhaseMask},
                  {"extended_phase_mask", detail.extendedPhaseMask},
                  {"logical_slot_count", detail.logicalSlotCount},
                  {"physical_slot_count", detail.physicalSlotCount},
                  {"slot_phase", detail.slotPhase},
                  {"slot_layout_kind", detail.slotLayoutKind},
                  {"compact_layout_evidence",
                   detail.compactLayoutEvidence},
                  {"extended_layout_evidence",
                   detail.extendedLayoutEvidence},
              }},
         }},
    };
    return report.dump();
}

CoordinateFailureReportPolicy::CoordinateFailureReportPolicy(
    CoordinateFailureReportPolicyOptions options)
    : options_(std::move(options)) {
    if (options_.persistenceWindow.count() < 0) {
        options_.persistenceWindow = std::chrono::milliseconds(0);
    }
    if (options_.minimumReportInterval.count() < 0) {
        options_.minimumReportInterval = std::chrono::milliseconds(0);
    }
    if (options_.duplicateWindow.count() < 0) {
        options_.duplicateWindow = std::chrono::milliseconds(0);
    }
    options_.maximumRememberedFingerprints =
        std::clamp<std::size_t>(
            options_.maximumRememberedFingerprints, 1U, 128U);
}

bool CoordinateFailureReportPolicy::Observe(
    const game::RuntimeStatus& status,
    std::string_view fingerprint,
    std::chrono::steady_clock::time_point now) {
    if (!successCounterInitialized_ ||
        status.coordinateSuccesses != observedSuccesses_) {
        observedSuccesses_ = status.coordinateSuccesses;
        successCounterInitialized_ = true;
        candidateFingerprint_.clear();
        candidateSince_ = {};
        return false;
    }
    if (!IsFailureActive(status) ||
        !IsValidFingerprint(fingerprint)) {
        candidateFingerprint_.clear();
        candidateSince_ = {};
        return false;
    }
    if (candidateFingerprint_ != fingerprint) {
        candidateFingerprint_ = std::string(fingerprint);
        candidateSince_ = now;
        return false;
    }
    if (now - candidateSince_ < options_.persistenceWindow) {
        return false;
    }

    reportedFingerprints_.erase(
        std::remove_if(
            reportedFingerprints_.begin(),
            reportedFingerprints_.end(),
            [&](const ReportedFingerprint& reported) {
                return now - reported.reportedAt >=
                    options_.duplicateWindow;
            }),
        reportedFingerprints_.end());
    if (std::any_of(
            reportedFingerprints_.begin(),
            reportedFingerprints_.end(),
            [&](const ReportedFingerprint& reported) {
                return reported.value == fingerprint;
            })) {
        return false;
    }
    if (lastReportAt_ != std::chrono::steady_clock::time_point{} &&
        now - lastReportAt_ < options_.minimumReportInterval) {
        return false;
    }

    lastReportAt_ = now;
    reportedFingerprints_.push_back(
        {std::string(fingerprint), now});
    if (reportedFingerprints_.size() >
        options_.maximumRememberedFingerprints) {
        reportedFingerprints_.erase(reportedFingerprints_.begin());
    }
    return true;
}

void CoordinateFailureReportPolicy::Reset() noexcept {
    candidateFingerprint_.clear();
    candidateSince_ = {};
    observedSuccesses_ = 0;
    successCounterInitialized_ = false;
}

struct CoordinateFailureUploader::Impl final {
    struct Report {
        std::string fingerprint;
        std::string payload;
    };

    Impl(std::shared_ptr<T3HttpTransport> transportValue,
         CoordinateFailureMetadata metadataValue,
         CoordinateFailureUploaderOptions optionsValue)
        : transport(std::move(transportValue)),
          metadata(std::move(metadataValue)),
          options(std::move(optionsValue)),
          policy(options.reportPolicy) {
        if (transport == nullptr) {
            throw std::invalid_argument("coordinate upload transport is null");
        }
        if (!IsValidEndpoint(options.endpoint)) {
            throw std::invalid_argument("coordinate upload endpoint is invalid");
        }
        options.maximumPayloadBytes = std::clamp<std::size_t>(
            options.maximumPayloadBytes, 1024U, 64U * 1024U);
        options.maximumQueuedReports = std::clamp<std::size_t>(
            options.maximumQueuedReports, 1U, 16U);
        if (options.retryDelays.empty()) {
            options.retryDelays.push_back(std::chrono::milliseconds(0));
        }
        for (auto& delay : options.retryDelays) {
            delay = std::clamp(
                delay, std::chrono::milliseconds(0),
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::minutes(5)));
        }
        if (const std::optional<Report> pending = LoadPending()) {
            queue.push_back(*pending);
        }
        worker = std::thread([this] { WorkerMain(); });
    }

    ~Impl() { Stop(); }

    bool Submit(Report report) {
        if (!IsValidFingerprint(report.fingerprint) ||
            report.payload.empty() ||
            report.payload.size() > options.maximumPayloadBytes) {
            return false;
        }
        std::lock_guard lock(mutex);
        if (stopping ||
            report.fingerprint == activeFingerprint ||
            std::any_of(
                queue.begin(), queue.end(),
                [&](const Report& queued) {
                    return queued.fingerprint == report.fingerprint;
                })) {
            return false;
        }
        if (queue.size() >= options.maximumQueuedReports) {
            queue.pop_front();
        }
        queue.push_back(std::move(report));
        wake.notify_all();
        return true;
    }

    void Observe(const game::RuntimeStatus& status,
                 int gameVersionIndex,
                 int driverIndex) {
        if (!IsFailureActive(status)) {
            policy.Observe(
                status, {}, std::chrono::steady_clock::now());
            return;
        }
        const std::string fingerprint =
            BuildCoordinateFailureFingerprint(
                status, gameVersionIndex, driverIndex);
        if (!policy.Observe(
                status, fingerprint,
                std::chrono::steady_clock::now())) {
            return;
        }
        Submit({
            fingerprint,
            BuildCoordinateFailurePayload(
                status, metadata, gameVersionIndex, driverIndex),
        });
    }

    bool WaitUntilIdle(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        return idle.wait_for(lock, timeout, [this] {
            return queue.empty() && !sending;
        });
    }

    void Stop() noexcept {
        {
            std::lock_guard lock(mutex);
            if (stopping) {
                if (!worker.joinable()) return;
            } else {
                stopping = true;
            }
        }
        try {
            transport->cancelPendingRequests();
        } catch (...) {
        }
        wake.notify_all();
        if (worker.joinable()) worker.join();
    }

    bool WaitRetry(std::chrono::milliseconds delay) {
        if (delay.count() <= 0) return true;
        std::unique_lock lock(mutex);
        return !wake.wait_for(lock, delay, [this] { return stopping; });
    }

    void WorkerMain() noexcept {
        for (;;) {
            Report report;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [this] {
                    return stopping || !queue.empty();
                });
                if (stopping) {
                    if (!queue.empty()) {
                        report = std::move(queue.back());
                        queue.clear();
                        lock.unlock();
                        WritePending(report);
                        lock.lock();
                    }
                    sending = false;
                    activeFingerprint.clear();
                    idle.notify_all();
                    return;
                }
                report = std::move(queue.front());
                queue.pop_front();
                sending = true;
                activeFingerprint = report.fingerprint;
            }

            WritePending(report);
            bool uploaded = false;
            std::string lastError;
            std::size_t attempts = 0;
            for (const auto delay : options.retryDelays) {
                if (!WaitRetry(delay)) break;
                {
                    std::lock_guard lock(mutex);
                    if (stopping) break;
                }
                ++attempts;
                T3HttpTransportResult result;
                try {
                    transport->resetCancellation();
                    result = transport->post(
                        options.endpoint,
                        "application/json",
                        report.payload);
                } catch (const std::exception& exception) {
                    result.error = exception.what();
                } catch (...) {
                    result.error = "coordinate upload threw";
                }
                if (result.success) {
                    uploaded = true;
                    break;
                }
                lastError = SanitizeUploadError(std::move(result.error));
            }

            if (uploaded) {
                RemovePending();
                std::fprintf(
                    stderr,
                    "[coordinate-upload] status=success attempts=%zu\n",
                    attempts);
            } else {
                std::fprintf(
                    stderr,
                    "[coordinate-upload] status=pending attempts=%zu error=%s\n",
                    attempts,
                    lastError.empty() ? "cancelled" : lastError.c_str());
            }
            std::fflush(stderr);

            {
                std::lock_guard lock(mutex);
                sending = false;
                activeFingerprint.clear();
                if (queue.empty()) idle.notify_all();
            }
        }
    }

    std::optional<Report> LoadPending() const {
        if (options.pendingPath.empty()) return std::nullopt;
        std::ifstream stream(options.pendingPath, std::ios::binary);
        if (!stream) return std::nullopt;
        std::string magic;
        std::string fingerprint;
        if (!std::getline(stream, magic) ||
            !std::getline(stream, fingerprint) ||
            magic != kPendingMagic ||
            !IsValidFingerprint(fingerprint)) {
            return std::nullopt;
        }
        std::string payload{
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
        if (payload.empty() ||
            payload.size() > options.maximumPayloadBytes) {
            return std::nullopt;
        }
        return Report{std::move(fingerprint), std::move(payload)};
    }

    void WritePending(const Report& report) const noexcept {
        if (options.pendingPath.empty()) return;
        const std::string temporaryPath =
            options.pendingPath + ".tmp";
        {
            std::ofstream stream(
                temporaryPath,
                std::ios::binary | std::ios::trunc);
            if (!stream) return;
            stream << kPendingMagic << '\n'
                   << report.fingerprint << '\n'
                   << report.payload;
            stream.flush();
            if (!stream) {
                stream.close();
                std::remove(temporaryPath.c_str());
                return;
            }
        }
#if defined(__ANDROID__)
        static_cast<void>(chmod(temporaryPath.c_str(), 0600));
#endif
        std::remove(options.pendingPath.c_str());
        if (std::rename(
                temporaryPath.c_str(),
                options.pendingPath.c_str()) != 0) {
            std::remove(temporaryPath.c_str());
        }
#if defined(__ANDROID__)
        static_cast<void>(chmod(options.pendingPath.c_str(), 0600));
#endif
    }

    void RemovePending() const noexcept {
        if (!options.pendingPath.empty()) {
            std::remove(options.pendingPath.c_str());
        }
    }

    std::shared_ptr<T3HttpTransport> transport;
    CoordinateFailureMetadata metadata;
    CoordinateFailureUploaderOptions options;
    CoordinateFailureReportPolicy policy;
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    std::deque<Report> queue;
    std::thread worker;
    std::string activeFingerprint;
    bool sending = false;
    bool stopping = false;
};

CoordinateFailureUploader::CoordinateFailureUploader(
    std::shared_ptr<T3HttpTransport> transport,
    CoordinateFailureMetadata metadata,
    CoordinateFailureUploaderOptions options)
    : impl_(std::make_unique<Impl>(
          std::move(transport),
          std::move(metadata),
          std::move(options))) {}

CoordinateFailureUploader::~CoordinateFailureUploader() = default;

void CoordinateFailureUploader::Observe(
    const game::RuntimeStatus& status,
    int gameVersionIndex,
    int driverIndex) {
    impl_->Observe(status, gameVersionIndex, driverIndex);
}

void CoordinateFailureUploader::ResetObservation() noexcept {
    impl_->policy.Reset();
}

bool CoordinateFailureUploader::Submit(
    std::string fingerprint,
    std::string payload) {
    return impl_->Submit(
        {std::move(fingerprint), std::move(payload)});
}

bool CoordinateFailureUploader::WaitUntilIdle(
    std::chrono::milliseconds timeout) {
    return impl_->WaitUntilIdle(timeout);
}

void CoordinateFailureUploader::Stop() noexcept {
    if (impl_ != nullptr) impl_->Stop();
}

}  // namespace lengjing::diagnostics
