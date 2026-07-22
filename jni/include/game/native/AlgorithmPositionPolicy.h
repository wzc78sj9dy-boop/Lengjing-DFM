#pragma once

#include "game/native/AlgorithmPositionRuntime.h"

#include <chrono>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace lengjing::game::native {

inline constexpr auto kAlgorithmPositionHistoryLifetime =
    std::chrono::milliseconds(1500);
inline constexpr double kAlgorithmPositionFirstDistanceSquared = 2250000.0;
inline constexpr double kAlgorithmPositionSecondDistanceSquared = 250000.0;
inline constexpr std::size_t kAlgorithmPositionCoordinatePageLimit = 200;
inline constexpr std::size_t kAlgorithmPositionMaximumCachedPages = 4096;
inline constexpr std::size_t kAlgorithmPositionRetainedCachedPages = 3072;
inline constexpr std::size_t kAlgorithmPositionRefreshBatchSize = 1024;

struct AlgorithmPositionRefreshPlan {
    bool first = false;
    bool discarded = true;
    bool candidate = false;
};

constexpr AlgorithmPositionRefreshPlan MakeAlgorithmPositionRefreshPlan(
    bool refreshFirst) noexcept {
    return AlgorithmPositionRefreshPlan{refreshFirst, true, false};
}

constexpr bool ShouldClearAlgorithmPositionCoordinatePages(
    std::size_t pageCount) noexcept {
    return pageCount > kAlgorithmPositionCoordinatePageLimit;
}

constexpr bool IsSupportedAlgorithmPositionSvc(
    std::uint64_t number) noexcept {
    return number == 25 || number == 29 || number == 62 || number == 98 ||
        number == 172 || number == 178 || number == 278;
}

struct AlgorithmPositionHistorySample {
    AlgorithmPosition position{};
    std::chrono::steady_clock::time_point capturedAt{};
};

enum class AlgorithmPositionFirstDecision {
    AcceptFirst,
    Rerun,
};

enum class AlgorithmPositionSecondDecision {
    AcceptSecond,
    NeedsCrossFrameConfirmation,
    FallbackHistory,
};

inline double AlgorithmPositionDistanceSquared(
    const AlgorithmPosition& left,
    const AlgorithmPosition& right) noexcept {
    const double x = static_cast<double>(left.x) - right.x;
    const double y = static_cast<double>(left.y) - right.y;
    const double z = static_cast<double>(left.z) - right.z;
    return std::fma(z, z, std::fma(x, x, y * y));
}

inline AlgorithmPositionFirstDecision EvaluateAlgorithmPositionFirst(
    const AlgorithmPosition& history,
    const AlgorithmPosition& first) noexcept {
    return AlgorithmPositionDistanceSquared(history, first) >
            kAlgorithmPositionFirstDistanceSquared
        ? AlgorithmPositionFirstDecision::Rerun
        : AlgorithmPositionFirstDecision::AcceptFirst;
}

inline AlgorithmPositionSecondDecision EvaluateAlgorithmPositionSecond(
    const AlgorithmPosition& history,
    const AlgorithmPosition& first,
    const AlgorithmPosition* second) noexcept {
    if (second == nullptr) {
        return AlgorithmPositionSecondDecision::FallbackHistory;
    }
    if (AlgorithmPositionDistanceSquared(history, *second) <=
        kAlgorithmPositionSecondDistanceSquared) {
        return AlgorithmPositionSecondDecision::AcceptSecond;
    }
    if (AlgorithmPositionDistanceSquared(first, *second) <=
        kAlgorithmPositionSecondDistanceSquared) {
        return AlgorithmPositionSecondDecision::NeedsCrossFrameConfirmation;
    }
    return AlgorithmPositionSecondDecision::FallbackHistory;
}

inline constexpr auto kAlgorithmPositionPendingMaximumAge =
    std::chrono::milliseconds(250);
inline constexpr std::uint64_t kAlgorithmPositionPendingMaximumFrameGap = 2;
inline constexpr double kAlgorithmPositionPendingDirectionCosine = 0.75;
inline constexpr double kAlgorithmPositionPendingMinimumStepRatio = 0.5;
inline constexpr double kAlgorithmPositionPendingMaximumStepRatio = 2.0;

struct AlgorithmPositionPendingSample {
    AlgorithmPosition first{};
    AlgorithmPosition last{};
    std::uint64_t firstFrame = 0;
    std::uint64_t lastFrame = 0;
    std::chrono::steady_clock::time_point firstAt{};
    std::chrono::steady_clock::time_point lastAt{};
    std::size_t count = 0;

    void Clear() noexcept { *this = {}; }
};

enum class AlgorithmPositionPendingDecision {
    Pending,
    Confirmed,
};

enum class AlgorithmPositionOutputDecision {
    AcceptImmediate,
    RetainHistory,
    AcceptConfirmed,
};

struct AlgorithmPositionOutputObservation {
    AlgorithmPositionOutputDecision decision =
        AlgorithmPositionOutputDecision::RetainHistory;
    double distanceSquared = 0.0;
    std::size_t pendingCount = 0;
};

inline bool IsAlgorithmPositionPendingFresh(
    const AlgorithmPositionPendingSample& sample,
    std::uint64_t frame,
    std::chrono::steady_clock::time_point now) noexcept {
    return sample.count != 0 && frame > sample.lastFrame &&
        frame - sample.lastFrame <= kAlgorithmPositionPendingMaximumFrameGap &&
        now >= sample.lastAt &&
        now - sample.lastAt <= kAlgorithmPositionPendingMaximumAge;
}

inline double AlgorithmPositionLengthSquared(
    const AlgorithmPosition& value) noexcept {
    return std::fma(
        static_cast<double>(value.z), static_cast<double>(value.z),
        std::fma(
            static_cast<double>(value.x), static_cast<double>(value.x),
            static_cast<double>(value.y) * static_cast<double>(value.y)));
}

inline double AlgorithmPositionDirectionCosine(
    const AlgorithmPosition& left,
    const AlgorithmPosition& right) noexcept {
    const double leftLength = AlgorithmPositionLengthSquared(left);
    const double rightLength = AlgorithmPositionLengthSquared(right);
    if (leftLength <= 0.0 || rightLength <= 0.0) return -1.0;
    const double dot =
        static_cast<double>(left.x) * right.x +
        static_cast<double>(left.y) * right.y +
        static_cast<double>(left.z) * right.z;
    return dot / std::sqrt(leftLength * rightLength);
}

inline AlgorithmPositionPendingDecision ObserveAlgorithmPositionPending(
    AlgorithmPositionPendingSample& sample,
    const AlgorithmPosition& candidate,
    std::uint64_t frame,
    std::chrono::steady_clock::time_point now) noexcept {
    if (!IsAlgorithmPositionPendingFresh(sample, frame, now)) {
        sample.Clear();
    }
    if (sample.count == 0) {
        sample.first = candidate;
        sample.last = candidate;
        sample.firstFrame = frame;
        sample.lastFrame = frame;
        sample.firstAt = now;
        sample.lastAt = now;
        sample.count = 1;
        return AlgorithmPositionPendingDecision::Pending;
    }

    const double distanceFromLast =
        AlgorithmPositionDistanceSquared(sample.last, candidate);
    if (distanceFromLast <= kAlgorithmPositionSecondDistanceSquared) {
        sample.Clear();
        return AlgorithmPositionPendingDecision::Confirmed;
    }

    if (sample.count == 1) {
        sample.last = candidate;
        sample.lastFrame = frame;
        sample.lastAt = now;
        sample.count = 2;
        return AlgorithmPositionPendingDecision::Pending;
    }

    const AlgorithmPosition step{
        sample.last.x - sample.first.x,
        sample.last.y - sample.first.y,
        sample.last.z - sample.first.z,
    };
    const AlgorithmPosition currentStep{
        candidate.x - sample.last.x,
        candidate.y - sample.last.y,
        candidate.z - sample.last.z,
    };
    const double previousStepLength = std::sqrt(
        AlgorithmPositionLengthSquared(step));
    const double currentStepLength = std::sqrt(
        AlgorithmPositionLengthSquared(currentStep));
    const double ratio = previousStepLength > 0.0
        ? currentStepLength / previousStepLength
        : 0.0;
    const double direction = AlgorithmPositionDirectionCosine(
        step, currentStep);
    const std::uint64_t frameSpan =
        sample.lastFrame > sample.firstFrame
        ? sample.lastFrame - sample.firstFrame
        : 1;
    const std::uint64_t currentSpan = frame > sample.lastFrame
        ? frame - sample.lastFrame
        : 1;
    const double scale = static_cast<double>(currentSpan) /
        static_cast<double>(frameSpan);
    const AlgorithmPosition expected{
        sample.last.x + step.x * static_cast<float>(scale),
        sample.last.y + step.y * static_cast<float>(scale),
        sample.last.z + step.z * static_cast<float>(scale),
    };
    const bool linear =
        AlgorithmPositionDistanceSquared(expected, candidate) <=
            kAlgorithmPositionSecondDistanceSquared &&
        direction >= kAlgorithmPositionPendingDirectionCosine &&
        ratio >= kAlgorithmPositionPendingMinimumStepRatio &&
        ratio <= kAlgorithmPositionPendingMaximumStepRatio;
    if (linear) {
        sample.Clear();
        return AlgorithmPositionPendingDecision::Confirmed;
    }

    sample.first = candidate;
    sample.last = candidate;
    sample.firstFrame = frame;
    sample.lastFrame = frame;
    sample.firstAt = now;
    sample.lastAt = now;
    sample.count = 1;
    return AlgorithmPositionPendingDecision::Pending;
}

inline AlgorithmPositionOutputObservation ObserveAlgorithmPositionOutput(
    const AlgorithmPosition& history,
    const AlgorithmPosition& candidate,
    AlgorithmPositionPendingSample& pending,
    std::uint64_t frame,
    std::chrono::steady_clock::time_point now) noexcept {
    AlgorithmPositionOutputObservation observation{};
    observation.distanceSquared =
        AlgorithmPositionDistanceSquared(history, candidate);
    if (observation.distanceSquared <=
        kAlgorithmPositionFirstDistanceSquared) {
        pending.Clear();
        observation.decision =
            AlgorithmPositionOutputDecision::AcceptImmediate;
        return observation;
    }

    const AlgorithmPositionPendingDecision pendingDecision =
        ObserveAlgorithmPositionPending(pending, candidate, frame, now);
    observation.pendingCount = pending.count;
    observation.decision = pendingDecision ==
            AlgorithmPositionPendingDecision::Confirmed
        ? AlgorithmPositionOutputDecision::AcceptConfirmed
        : AlgorithmPositionOutputDecision::RetainHistory;
    return observation;
}

class AlgorithmPositionResultCache final {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    bool Lookup(std::uintptr_t entityAddress,
                TimePoint now,
                AlgorithmPositionHistorySample& sample) {
        const auto found = samples_.find(entityAddress);
        if (found == samples_.end()) return false;
        if (!IsFresh(found->second, now)) {
            samples_.erase(found);
            return false;
        }
        sample = found->second;
        return true;
    }

    void Store(std::uintptr_t entityAddress,
               const AlgorithmPosition& position,
               TimePoint now) {
        samples_[entityAddress] = AlgorithmPositionHistorySample{
            position,
            now,
        };
    }

    void Clear() noexcept { samples_.clear(); }

    static bool IsFresh(const AlgorithmPositionHistorySample& sample,
                        TimePoint now) noexcept {
        return now >= sample.capturedAt &&
            now - sample.capturedAt <= kAlgorithmPositionHistoryLifetime;
    }

private:
    std::unordered_map<std::uintptr_t, AlgorithmPositionHistorySample>
        samples_;
};

}  // namespace lengjing::game::native
