#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <unordered_set>

namespace lengjing::game::aim {

struct TrackingPoint {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct TrackingCommand {
    float pitch = 0.0f;
    float yaw = 0.0f;
    std::int32_t flag = 0;
};

struct TrackingPrediction {
    TrackingPoint point{};
    std::int32_t distanceMeters = 0;
    bool valid = false;
};

static_assert(sizeof(TrackingCommand) == 12);
static_assert(alignof(TrackingCommand) == alignof(float));

class TrackingCalculator final {
public:
    static TrackingCommand Calculate(
        bool enabled,
        const TrackingPoint& origin,
        const TrackingPoint& target,
        const TrackingPoint& velocity,
        float projectileSpeed,
        float gravity) noexcept
    {
        if (!enabled) return {};
        const TrackingPrediction prediction = Predict(
            origin, target, velocity, projectileSpeed, gravity);
        if (!prediction.valid) return {};
        return CalculateAngles(enabled, origin, prediction.point);
    }

    static TrackingCommand CalculateAngles(
        bool enabled,
        const TrackingPoint& origin,
        const TrackingPoint& target) noexcept
    {
        if (!enabled || !IsFinite(origin) || !IsFinite(target)) return {};

        const float aimX = target.x - origin.x;
        const float aimY = target.y - origin.y;
        const float aimZ = target.z - origin.z;
        if (!std::isfinite(aimX)
            || !std::isfinite(aimY)
            || !std::isfinite(aimZ)) {
            return {};
        }

        const double aimXDouble = static_cast<double>(aimX);
        const double aimYDouble = static_cast<double>(aimY);
        const double horizontal = std::sqrt(std::fma(
            aimYDouble,
            aimYDouble,
            aimXDouble * aimXDouble));
        const double pitch = std::atan2(
            static_cast<double>(aimZ), horizontal) * DegreesPerRadian();
        const float yawRadians = ::atan2f(aimY, aimX);
        const double yaw =
            static_cast<double>(yawRadians) * DegreesPerRadian();
        if (!std::isfinite(pitch) || !std::isfinite(yaw)) return {};

        const float pitchValue = static_cast<float>(pitch);
        const float yawValue = static_cast<float>(yaw);
        if (!std::isfinite(pitchValue) || !std::isfinite(yawValue)) return {};
        return TrackingCommand{pitchValue, yawValue, 1};
    }

    static TrackingPrediction Predict(
        const TrackingPoint& origin,
        const TrackingPoint& target,
        const TrackingPoint& velocity,
        float projectileSpeed,
        float gravity) noexcept
    {
        if (!IsFinite(origin)
            || !IsFinite(target)
            || !IsFinite(velocity)
            || !std::isfinite(projectileSpeed)
            || projectileSpeed <= 0.0f
            || !std::isfinite(gravity)) {
            return {};
        }

        const float distanceXFloat = target.x - origin.x;
        const float distanceYFloat = target.y - origin.y;
        const float distanceZFloat = target.z - origin.z;
        const double distanceX = static_cast<double>(distanceXFloat);
        const double distanceY = static_cast<double>(distanceYFloat);
        const double distanceZ = static_cast<double>(distanceZFloat);
        double distanceSquared = distanceX * distanceX;
        distanceSquared = std::fma(
            distanceY, distanceY, distanceSquared);
        distanceSquared = std::fma(
            distanceZ, distanceZ, distanceSquared);
        const double distance = std::sqrt(distanceSquared);
        if (!std::isfinite(distance)
            || distance > static_cast<double>(std::numeric_limits<int>::max())) {
            return {};
        }

        const std::int32_t distanceMeters =
            static_cast<std::int32_t>(distance) / 100;
        const float flightTime =
            static_cast<float>(distanceMeters) / projectileSpeed;
        if (!std::isfinite(flightTime)) return {};

        TrackingPoint predicted = target;
        if (velocity.x != 0.0f || velocity.y != 0.0f ||
            velocity.z != 0.0f) {
            predicted.x = std::fma(flightTime, velocity.x, target.x);
            predicted.y = std::fma(flightTime, velocity.y, target.y);
            const double timeSquared =
                static_cast<double>(flightTime) *
                static_cast<double>(flightTime);
            const double verticalScale =
                static_cast<double>(gravity) * 22.5;
            predicted.z = static_cast<float>(std::fma(
                timeSquared,
                verticalScale,
                static_cast<double>(target.z)));
        }
        if (!IsFinite(predicted)) return {};
        return TrackingPrediction{predicted, distanceMeters, true};
    }

private:
    static bool IsFinite(const TrackingPoint& point) noexcept
    {
        return std::isfinite(point.x)
            && std::isfinite(point.y)
            && std::isfinite(point.z);
    }

    static double DegreesPerRadian() noexcept
    {
        constexpr std::uint64_t bits = 0x404CA5DC1A63C0B1ULL;
        double value = 0.0;
        static_assert(sizeof(value) == sizeof(bits));
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
};

using SeededSelectionCallback =
    std::function<std::uint32_t(std::uint32_t)>;

struct HitSelectionDecision {
    bool accepted = false;
    bool validInput = false;
    bool selectionUnreachable = false;
    bool cacheRebuilt = false;
};

class HitSelectionCache final {
public:
    static std::int32_t SelectedCountForPercentage(
        std::int32_t percentage,
        std::int32_t totalCount) noexcept
    {
        const std::uint32_t productBits =
            static_cast<std::uint32_t>(percentage)
            * static_cast<std::uint32_t>(totalCount);
        std::int32_t signedProduct = 0;
        static_assert(sizeof(signedProduct) == sizeof(productBits));
        std::memcpy(&signedProduct, &productBits, sizeof(signedProduct));
        return signedProduct / 100;
    }

    HitSelectionDecision Evaluate(
        std::int32_t selectedCount,
        std::int32_t currentIndex,
        std::int32_t totalCount,
        const SeededSelectionCallback& nextValue)
    {
        HitSelectionDecision decision{};
        if (static_cast<std::uint32_t>(totalCount) > 100U
            || currentIndex > totalCount) {
            return decision;
        }

        if (currentIndex == totalCount) {
            decision.validInput = true;
            decision.accepted = true;
            return decision;
        }

        decision.validInput = true;
        if (selectedCount < 0) {
            decision.validInput = false;
            return decision;
        }
        if (selectedCount > totalCount) {
            decision.selectionUnreachable = true;
            return decision;
        }

        const bool rebuild =
            cachedSelectedCount_ != selectedCount
            || cachedTotalCount_ != totalCount;
        if (!rebuild) {
            decision.accepted = Contains(currentIndex);
            return decision;
        }
        if (!nextValue && selectedCount != 0) {
            decision.validInput = false;
            return decision;
        }

        selectedIndexes_.clear();
        std::uint32_t seed = 1;
        std::size_t attempts = 0;
        while (selectedIndexes_.size()
               < static_cast<std::size_t>(selectedCount)) {
            const std::uint32_t candidate =
                nextValue(seed) % static_cast<std::uint32_t>(totalCount);
            selectedIndexes_.insert(candidate);
            ++attempts;
            if (attempts >= kMaximumSelectionAttempts) {
                decision.selectionUnreachable = true;
                selectedIndexes_.clear();
                return decision;
            }
            ++seed;
        }

        cachedSelectedCount_ = selectedCount;
        cachedTotalCount_ = totalCount;
        decision.cacheRebuilt = true;
        decision.accepted = Contains(currentIndex);
        return decision;
    }

    void Reset() noexcept
    {
        cachedSelectedCount_.reset();
        cachedTotalCount_.reset();
        selectedIndexes_.clear();
    }

private:
    bool Contains(std::int32_t currentIndex) const
    {
        return currentIndex >= 0
            && selectedIndexes_.find(static_cast<std::uint32_t>(currentIndex))
                != selectedIndexes_.end();
    }

    static constexpr std::size_t kMaximumSelectionAttempts = 4096;
    std::optional<std::int32_t> cachedSelectedCount_{};
    std::optional<std::int32_t> cachedTotalCount_{};
    std::unordered_set<std::uint32_t> selectedIndexes_{};
};

}  // namespace lengjing::game::aim
