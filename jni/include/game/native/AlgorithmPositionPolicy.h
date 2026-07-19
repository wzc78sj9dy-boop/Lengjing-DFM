#pragma once

#include "game/native/AlgorithmPositionRuntime.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace lengjing::game::native {

inline constexpr auto kAlgorithmPositionHistoryLifetime =
    std::chrono::milliseconds(1500);
inline constexpr double kAlgorithmPositionFirstDistanceSquared = 2250000.0;
inline constexpr double kAlgorithmPositionSecondDistanceSquared = 250000.0;

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
    return AlgorithmPositionDistanceSquared(history, *second) <=
                kAlgorithmPositionSecondDistanceSquared ||
            AlgorithmPositionDistanceSquared(first, *second) <=
                kAlgorithmPositionSecondDistanceSquared
        ? AlgorithmPositionSecondDecision::AcceptSecond
        : AlgorithmPositionSecondDecision::FallbackHistory;
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
