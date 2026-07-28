#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace lengjing::game::native {

using StableCoordinate = std::array<float, 3>;

struct CoordinateStabilityState {
    StableCoordinate current{};
    std::array<StableCoordinate, 50> history{};
    std::uintptr_t componentGeneration = 0;
    std::uint32_t count = 0;
    std::uint32_t ringIndex = 0;
    std::uint32_t invalidStreak = 0;
    bool initialized = false;
};

enum class CoordinateStabilityDecision : std::uint8_t {
    Accepted,
    Rejected,
    InvalidRetained,
    InvalidCleared,
};

class CoordinateStabilityPolicy final {
public:
    static constexpr std::size_t kHistoryCapacity = 50;
    static constexpr std::uint32_t kInvalidStreakLimit = 60;
    static constexpr float kImmediateJumpSquared = 2250000.0f;
    static constexpr float kLatestJumpSquared = 122500.0f;

    static bool IsValid(const StableCoordinate& coordinate) noexcept {
        const float x = coordinate[0];
        const float y = coordinate[1];
        const float z = coordinate[2];
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
            return false;
        }
        if (x == 0.0f && y == 0.0f && z == 0.0f) {
            return false;
        }
        if (std::fabs(x) > 1000000.0f ||
            std::fabs(y) > 1000000.0f ||
            std::fabs(z) > 1000000.0f) {
            return false;
        }
        const float xySquared = x * x + y * y;
        return xySquared > 1.0f &&
            (std::fabs(z) > 1000.0f || xySquared > 100.0f);
    }

    static bool ShouldReject(
        const CoordinateStabilityState& state,
        std::uintptr_t component,
        const StableCoordinate& candidate) noexcept {
        if (!IsValid(candidate)) return true;
        if (!state.initialized ||
            state.invalidStreak >= kInvalidStreakLimit ||
            !IsValid(state.current)) {
            return false;
        }

        const float currentDistance =
            XyDistanceSquared(candidate, state.current);
        if (currentDistance > kImmediateJumpSquared) return true;
        if (state.count < 6 || state.componentGeneration != component) {
            return false;
        }

        const std::size_t ring =
            static_cast<std::size_t>(state.ringIndex) % kHistoryCapacity;
        const std::size_t latestIndex =
            (ring + kHistoryCapacity - 1) % kHistoryCapacity;
        const StableCoordinate& latest = state.history[latestIndex];
        if (!IsValid(latest)) return false;

        const float latestDistance = XyDistanceSquared(candidate, latest);
        float weightedSum = latestDistance;
        float weightSum = 1.0f;
        const std::size_t sampleCount = std::min<std::size_t>(
            state.count,
            kHistoryCapacity);

        for (std::size_t ordinal = 2;
             ordinal <= sampleCount;
             ++ordinal) {
            const std::size_t index =
                (ring + kHistoryCapacity - ordinal) % kHistoryCapacity;
            const StableCoordinate& sample = state.history[index];
            if (!IsValid(sample)) break;
            const float weight = 1.0f / static_cast<float>(ordinal);
            weightedSum += XyDistanceSquared(candidate, sample) * weight;
            weightSum += weight;
        }

        return weightSum > 0.0f &&
            weightedSum / weightSum > kImmediateJumpSquared &&
            latestDistance > kLatestJumpSquared;
    }

    static CoordinateStabilityDecision Submit(
        CoordinateStabilityState& state,
        std::uintptr_t component,
        const StableCoordinate& candidate) noexcept {
        PrepareComponent(state, component);
        if (!IsValid(candidate)) {
            return RecordInvalid(state, component);
        }
        if (ShouldReject(state, component, candidate)) {
            return AdvanceInvalidStreak(state, component)
                ? CoordinateStabilityDecision::InvalidCleared
                : CoordinateStabilityDecision::Rejected;
        }
        Append(state, component, candidate);
        return CoordinateStabilityDecision::Accepted;
    }

    static CoordinateStabilityDecision RecordInvalid(
        CoordinateStabilityState& state,
        std::uintptr_t component) noexcept {
        PrepareComponent(state, component);
        if (AdvanceInvalidStreak(state, component)) {
            return CoordinateStabilityDecision::InvalidCleared;
        }
        return CoordinateStabilityDecision::InvalidRetained;
    }

    static void Reset(
        CoordinateStabilityState& state,
        std::uintptr_t component = 0) noexcept {
        state = CoordinateStabilityState{};
        state.componentGeneration = component;
    }

private:
    static float XyDistanceSquared(
        const StableCoordinate& left,
        const StableCoordinate& right) noexcept {
        const float dx = left[0] - right[0];
        const float dy = left[1] - right[1];
        return dx * dx + dy * dy;
    }

    static void PrepareComponent(
        CoordinateStabilityState& state,
        std::uintptr_t component) noexcept {
        if (state.componentGeneration == component) return;
        if (state.initialized || state.count != 0) {
            state.componentGeneration = component;
            state.invalidStreak = 0;
            return;
        }
        Reset(state, component);
    }

    static bool AdvanceInvalidStreak(
        CoordinateStabilityState& state,
        std::uintptr_t component) noexcept {
        if (state.invalidStreak >= kInvalidStreakLimit - 1) {
            Reset(state, component);
            return true;
        }
        ++state.invalidStreak;
        return false;
    }

    static void Append(
        CoordinateStabilityState& state,
        std::uintptr_t component,
        const StableCoordinate& coordinate) noexcept {
        state.componentGeneration = component;
        const std::size_t index =
            static_cast<std::size_t>(state.ringIndex) % kHistoryCapacity;
        state.history[index] = coordinate;
        state.ringIndex = static_cast<std::uint32_t>(
            (index + 1) % kHistoryCapacity);
        if (state.count < kHistoryCapacity) ++state.count;
        state.current = coordinate;
        state.invalidStreak = 0;
        state.initialized = true;
    }
};

}
