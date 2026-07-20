#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace lengjing::game::native {

struct AlgorithmExecutionContextRefreshKey {
    std::int32_t processId = -1;
    std::uintptr_t moduleBase = 0;
    std::uintptr_t guestPc = 0;
    std::uint32_t entryInstruction = 0;
    bool coordinatePoolSelected = false;
};

constexpr bool operator==(
    const AlgorithmExecutionContextRefreshKey& left,
    const AlgorithmExecutionContextRefreshKey& right) noexcept {
    return left.processId == right.processId &&
        left.moduleBase == right.moduleBase &&
        left.guestPc == right.guestPc &&
        left.entryInstruction == right.entryInstruction &&
        left.coordinatePoolSelected == right.coordinatePoolSelected;
}

constexpr bool operator!=(
    const AlgorithmExecutionContextRefreshKey& left,
    const AlgorithmExecutionContextRefreshKey& right) noexcept {
    return !(left == right);
}

class AlgorithmExecutionContextRefreshPolicy final {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;
    using TimePoint = Clock::time_point;

    explicit AlgorithmExecutionContextRefreshPolicy(
        Duration successLifetime = std::chrono::milliseconds(100)) noexcept
        : successLifetime_(successLifetime) {}

    bool ShouldRefresh(const AlgorithmExecutionContextRefreshKey& key,
                       TimePoint now) const noexcept {
        return !ready_ || key_ != key || now >= refreshAt_;
    }

    void MarkSucceeded(const AlgorithmExecutionContextRefreshKey& key,
                       TimePoint now) noexcept {
        key_ = key;
        refreshAt_ = now + successLifetime_;
        ready_ = true;
    }

    void MarkFailed() noexcept {
        Invalidate();
    }

    void Invalidate() noexcept {
        key_ = AlgorithmExecutionContextRefreshKey{};
        refreshAt_ = TimePoint{};
        ready_ = false;
    }

private:
    Duration successLifetime_{};
    AlgorithmExecutionContextRefreshKey key_{};
    TimePoint refreshAt_{};
    bool ready_ = false;
};

class AlgorithmReplayBackoffPolicy final {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;
    using TimePoint = Clock::time_point;

    explicit AlgorithmReplayBackoffPolicy(
        Duration retryInterval = std::chrono::milliseconds(100),
        std::size_t failureFramesBeforeBackoff = 2) noexcept
        : retryInterval_(retryInterval),
          failureFramesBeforeBackoff_(
              failureFramesBeforeBackoff == 0
                  ? 1
                  : failureFramesBeforeBackoff) {}

    bool BeginFrame(TimePoint now) noexcept {
        if (!backingOff_) return true;
        if (now < nextProbeAt_) return false;
        nextProbeAt_ = now + retryInterval_;
        return true;
    }

    void ObserveFrame(std::size_t attempts,
                      std::size_t successes,
                      TimePoint now) noexcept {
        if (successes != 0) {
            MarkSucceeded();
            return;
        }
        if (attempts == 0 || backingOff_) return;
        ++consecutiveFailedFrames_;
        if (consecutiveFailedFrames_ < failureFramesBeforeBackoff_) return;
        backingOff_ = true;
        nextProbeAt_ = now + retryInterval_;
    }

    void MarkSucceeded() noexcept {
        consecutiveFailedFrames_ = 0;
        nextProbeAt_ = TimePoint{};
        backingOff_ = false;
    }

    void Reset() noexcept {
        MarkSucceeded();
    }

    bool IsBackingOff() const noexcept {
        return backingOff_;
    }

private:
    Duration retryInterval_{};
    std::size_t failureFramesBeforeBackoff_ = 1;
    std::size_t consecutiveFailedFrames_ = 0;
    TimePoint nextProbeAt_{};
    bool backingOff_ = false;
};

struct AlgorithmReplayPageKey {
    std::uintptr_t guestPc = 0;
    std::uint64_t tpidrEl0 = 0;
    std::int32_t threadId = 0;
    std::uint64_t threadStartTimeTicks = 0;
    std::uint64_t generation = 0;
};

constexpr bool operator==(const AlgorithmReplayPageKey& left,
                          const AlgorithmReplayPageKey& right) noexcept {
    return left.guestPc == right.guestPc &&
        left.tpidrEl0 == right.tpidrEl0 &&
        left.threadId == right.threadId &&
        left.threadStartTimeTicks == right.threadStartTimeTicks &&
        left.generation == right.generation;
}

constexpr bool operator!=(const AlgorithmReplayPageKey& left,
                          const AlgorithmReplayPageKey& right) noexcept {
    return !(left == right);
}

class AlgorithmReplayPagePolicy final {
public:
    void BeginFrame() noexcept {
        refreshPending_ = true;
    }

    bool ConsumeRefresh(const AlgorithmReplayPageKey& key) noexcept {
        if (!ready_ || key_ != key) {
            key_ = key;
            ready_ = true;
            refreshPending_ = true;
        }
        if (!refreshPending_) return false;
        refreshPending_ = false;
        return true;
    }

    void Invalidate() noexcept {
        key_ = AlgorithmReplayPageKey{};
        refreshPending_ = false;
        ready_ = false;
    }

private:
    AlgorithmReplayPageKey key_{};
    bool refreshPending_ = false;
    bool ready_ = false;
};

}  // namespace lengjing::game::native
