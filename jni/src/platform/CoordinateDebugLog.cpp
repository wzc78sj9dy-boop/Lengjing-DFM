#include "platform/CoordinateDebugLog.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace lengjing::platform {

CoordinateDebugLogTransition CoordinateDebugLogSessionPolicy::UpdateRequested(
    bool requested, TimePoint now) noexcept {
    if (requested == requested_) return CoordinateDebugLogTransition::None;
    requested_ = requested;
    if (!requested) {
        active_ = false;
        return CoordinateDebugLogTransition::Stopped;
    }
    active_ = true;
    startedAt_ = now;
    bytesWritten_ = 0;
    return CoordinateDebugLogTransition::Started;
}

bool CoordinateDebugLogSessionPolicy::IsActive(TimePoint now) noexcept {
    if (active_ &&
        (now - startedAt_ >= kDuration ||
         bytesWritten_ >= kMaximumBytes)) {
        active_ = false;
    }
    return active_;
}

std::size_t CoordinateDebugLogSessionPolicy::RemainingBytes(
    TimePoint now) noexcept {
    if (!IsActive(now)) return 0;
    return kMaximumBytes - bytesWritten_;
}

void CoordinateDebugLogSessionPolicy::RecordBytes(
    std::size_t bytes) noexcept {
    const std::size_t remaining = kMaximumBytes -
        std::min(bytesWritten_, kMaximumBytes);
    bytesWritten_ += std::min(bytes, remaining);
    if (bytesWritten_ >= kMaximumBytes) active_ = false;
}

void CoordinateDebugLogSessionPolicy::StopWriting() noexcept {
    active_ = false;
}

namespace {

constexpr std::size_t kMaximumPathLength = 1024;
constexpr std::size_t kMaximumVersionLength = 64;
constexpr std::size_t kFormatBufferSize = 8192;
constexpr std::size_t kFileBufferSize = 256U * 1024U;
constexpr auto kFlushInterval = std::chrono::seconds(1);
constexpr char kSizeLimitMarker[] =
    "[coordinate-debug-stop] reason=size_limit\n";

struct CoordinateDebugLogState {
    std::mutex mutex;
    CoordinateDebugLogSessionPolicy policy;
    std::FILE* file = nullptr;
    std::array<char, kMaximumPathLength> path{};
    std::array<char, kMaximumVersionLength> version{};
    std::array<char, kFileBufferSize> fileBuffer{};
    CoordinateDebugLogSessionPolicy::TimePoint lastFlushAt{};
    bool configured = false;
};

CoordinateDebugLogState gState;
std::atomic_bool gActive{false};
using CoordinateDebugLogTick =
    CoordinateDebugLogSessionPolicy::Clock::duration::rep;
std::atomic<CoordinateDebugLogTick> gDeadlineTick{0};

CoordinateDebugLogTick ToTick(
    CoordinateDebugLogSessionPolicy::TimePoint time) noexcept {
    return time.time_since_epoch().count();
}

int ProcessId() noexcept {
#if defined(_WIN32)
    return _getpid();
#else
    return static_cast<int>(getpid());
#endif
}

void CloseLogLocked() noexcept {
    if (gState.file == nullptr) return;
    std::fclose(gState.file);
    gState.file = nullptr;
}

void DeactivateLogLocked() noexcept {
    gState.policy.StopWriting();
    gActive.store(false, std::memory_order_release);
    gDeadlineTick.store(0, std::memory_order_release);
    CloseLogLocked();
}

bool SessionActiveLocked(
    CoordinateDebugLogSessionPolicy::TimePoint now) noexcept {
    if (gState.file != nullptr && gState.policy.IsActive(now)) return true;
    DeactivateLogLocked();
    return false;
}

bool WriteLocked(const char* data, std::size_t size) noexcept {
    const auto now = CoordinateDebugLogSessionPolicy::Clock::now();
    if (!SessionActiveLocked(now)) return false;
    const std::size_t remaining = gState.policy.RemainingBytes(now);
    if (size > remaining) {
        constexpr std::size_t markerSize = sizeof(kSizeLimitMarker) - 1;
        if (markerSize <= remaining) {
            const std::size_t written = std::fwrite(
                kSizeLimitMarker, 1, markerSize, gState.file);
            gState.policy.RecordBytes(written);
        }
        DeactivateLogLocked();
        return false;
    }
    const std::size_t written = std::fwrite(data, 1, size, gState.file);
    gState.policy.RecordBytes(written);
    const bool flushDue = now - gState.lastFlushAt >= kFlushInterval;
    const bool flushFailed = flushDue && std::fflush(gState.file) != 0;
    if (flushDue) gState.lastFlushAt = now;
    if (written != size ||
        flushFailed ||
        !gState.policy.IsActive(
            CoordinateDebugLogSessionPolicy::Clock::now())) {
        DeactivateLogLocked();
    }
    return written == size;
}

bool OpenSessionLocked() noexcept {
    CloseLogLocked();
    gState.file = std::fopen(gState.path.data(), "wb");
    if (gState.file == nullptr) return false;
    if (std::setvbuf(
            gState.file,
            gState.fileBuffer.data(),
            _IOFBF,
            gState.fileBuffer.size()) != 0) {
        CloseLogLocked();
        return false;
    }
    gState.lastFlushAt = CoordinateDebugLogSessionPolicy::Clock::now();

    std::array<char, 512> header{};
    const int size = std::snprintf(
        header.data(),
        header.size(),
        "[coordinate-debug-start] schema=4 version=%s pid=%d "
        "trace=1 candidates_full=0 slot_family_calibration=1 "
        "duration_seconds=30 max_bytes=%zu\n",
        gState.version.data(),
        ProcessId(),
        CoordinateDebugLogSessionPolicy::kMaximumBytes);
    const bool wroteHeader = size > 0 &&
        static_cast<std::size_t>(size) < header.size() &&
        WriteLocked(header.data(), static_cast<std::size_t>(size));
    return wroteHeader && std::fflush(gState.file) == 0;
}

}  // namespace

bool ConfigureCoordinateDebugLog(
    const char* path, const char* version) noexcept {
    if (path == nullptr || version == nullptr) return false;
    const std::size_t pathLength = std::strlen(path);
    const std::size_t versionLength = std::strlen(version);
    if (pathLength == 0 || pathLength >= kMaximumPathLength ||
        versionLength == 0 || versionLength >= kMaximumVersionLength) {
        return false;
    }

    gActive.store(false, std::memory_order_release);
    gDeadlineTick.store(0, std::memory_order_release);
    std::lock_guard<std::mutex> lock(gState.mutex);
    CloseLogLocked();
    gState.policy = CoordinateDebugLogSessionPolicy{};
    std::memcpy(gState.path.data(), path, pathLength + 1);
    std::memcpy(gState.version.data(), version, versionLength + 1);
    gState.configured = true;
    return true;
}

void UpdateCoordinateDebugLogSession(bool requested) noexcept {
    if (!requested) {
        gActive.store(false, std::memory_order_release);
        gDeadlineTick.store(0, std::memory_order_release);
    }
    std::lock_guard<std::mutex> lock(gState.mutex);
    const auto now = CoordinateDebugLogSessionPolicy::Clock::now();
    const CoordinateDebugLogTransition transition =
        gState.policy.UpdateRequested(requested, now);
    if (transition == CoordinateDebugLogTransition::Stopped) {
        CloseLogLocked();
        return;
    }
    if (transition != CoordinateDebugLogTransition::Started) return;
    if (!gState.configured || !OpenSessionLocked()) {
        gState.policy.StopWriting();
        CloseLogLocked();
        return;
    }
    gDeadlineTick.store(
        ToTick(now + CoordinateDebugLogSessionPolicy::kDuration),
        std::memory_order_release);
    gActive.store(true, std::memory_order_release);
}

bool CoordinateDebugLogActive() noexcept {
    if (!gActive.load(std::memory_order_acquire)) return false;
    const auto now = CoordinateDebugLogSessionPolicy::Clock::now();
    if (ToTick(now) <
        gDeadlineTick.load(std::memory_order_acquire)) {
        return true;
    }
    std::lock_guard<std::mutex> lock(gState.mutex);
    return SessionActiveLocked(now);
}

void CoordinateDebugLogPrint(const char* format, ...) noexcept {
    if (format == nullptr ||
        !gActive.load(std::memory_order_acquire)) {
        return;
    }

    std::array<char, kFormatBufferSize> buffer{};
    va_list arguments;
    va_start(arguments, format);
    const int result = std::vsnprintf(
        buffer.data(), buffer.size(), format, arguments);
    va_end(arguments);
    if (result <= 0) return;

    const std::size_t size = std::min(
        static_cast<std::size_t>(result), buffer.size() - 1);
    std::lock_guard<std::mutex> lock(gState.mutex);
    WriteLocked(buffer.data(), size);
}

}  // namespace lengjing::platform
