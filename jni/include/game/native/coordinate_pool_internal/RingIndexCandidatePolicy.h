#pragma once

#include "game/native/coordinate_pool_internal/Expr.h"

#include <cstdint>
#include <exception>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lengjing::game::native::coordinate_pool_internal::coord_dec {

struct RingIndexSuccessorRelation {
    int currentCandidate = -1;
    std::uint32_t modulus = 0;
};

inline std::uint64_t MixRingIndexProbeValue(std::uint64_t value) noexcept {
    value ^= value >> 30;
    value *= UINT64_C(0xBF58476D1CE4E5B9);
    value ^= value >> 27;
    value *= UINT64_C(0x94D049BB133111EB);
    return value ^ (value >> 31);
}

inline std::uint64_t HashRingIndexProbeName(
    const std::string& name) noexcept {
    std::uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char character : name) {
        hash ^= character;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

inline RingIndexSuccessorRelation DetectRingIndexSuccessorRelation(
    const std::shared_ptr<Expr>& first,
    const std::shared_ptr<Expr>& second,
    const std::set<std::string>& dependencies) {
    if (!first || !second || dependencies.empty()) {
        return {};
    }

    std::vector<std::pair<std::uint64_t, std::uint64_t>> samples;
    samples.reserve(64);
    try {
        for (std::uint64_t sample = 0; sample < 64; ++sample) {
            std::unordered_map<std::string, std::uint64_t> values;
            values.reserve(dependencies.size());
            for (const auto& dependency : dependencies) {
                const std::uint64_t seed =
                    HashRingIndexProbeName(dependency) ^
                    (sample * UINT64_C(0x9E3779B97F4A7C15));
                values.emplace(
                    dependency,
                    MixRingIndexProbeValue(seed));
            }
            samples.emplace_back(
                first->eval(values),
                second->eval(values));
        }
    } catch (const std::exception&) {
        return {};
    }

    const auto successorModulus = [&](bool firstBeforeSecond) {
        for (std::uint32_t modulus = 2; modulus <= 32; ++modulus) {
            std::set<std::uint64_t> observed;
            bool matches = true;
            for (const auto& sample : samples) {
                const std::uint64_t current =
                    firstBeforeSecond ? sample.first : sample.second;
                const std::uint64_t next =
                    firstBeforeSecond ? sample.second : sample.first;
                if (current >= modulus || next >= modulus ||
                    next != (current + 1) % modulus) {
                    matches = false;
                    break;
                }
                observed.insert(current);
            }
            if (matches && observed.size() >= 3) {
                return modulus;
            }
        }
        return std::uint32_t{0};
    };

    const std::uint32_t firstModulus = successorModulus(true);
    const std::uint32_t secondModulus = successorModulus(false);
    if ((firstModulus == 0) == (secondModulus == 0)) {
        return {};
    }
    return firstModulus != 0
        ? RingIndexSuccessorRelation{0, firstModulus}
        : RingIndexSuccessorRelation{1, secondModulus};
}

}  // namespace lengjing::game::native::coordinate_pool_internal::coord_dec
