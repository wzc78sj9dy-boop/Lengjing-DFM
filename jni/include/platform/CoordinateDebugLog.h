#pragma once

#include <chrono>
#include <cstddef>

namespace lengjing::platform {

enum class CoordinateDebugLogTransition {
    None,
    Started,
    Stopped,
};

class CoordinateDebugLogSessionPolicy {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    static constexpr auto kDuration = std::chrono::seconds(30);
    static constexpr std::size_t kMaximumBytes = 32U * 1024U * 1024U;

    CoordinateDebugLogTransition UpdateRequested(
        bool requested, TimePoint now) noexcept;
    bool IsActive(TimePoint now) noexcept;
    std::size_t RemainingBytes(TimePoint now) noexcept;
    void RecordBytes(std::size_t bytes) noexcept;
    void StopWriting() noexcept;

    bool Requested() const noexcept { return requested_; }
    std::size_t BytesWritten() const noexcept { return bytesWritten_; }

private:
    bool requested_ = false;
    bool active_ = false;
    TimePoint startedAt_{};
    std::size_t bytesWritten_ = 0;
};

bool ConfigureCoordinateDebugLog(
    const char* path, const char* version) noexcept;
void UpdateCoordinateDebugLogSession(bool requested) noexcept;
bool CoordinateDebugLogActive() noexcept;

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
void CoordinateDebugLogPrint(const char* format, ...) noexcept;

}  // namespace lengjing::platform
