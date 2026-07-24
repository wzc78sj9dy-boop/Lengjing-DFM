#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace lengjing::game::native {

inline constexpr std::size_t kCoordinateEntryMaximumDirectBranchHops = 8;

enum class CoordinateEntryBranchDecodeStatus : std::uint8_t {
    NotBranch,
    Resolved,
    InvalidAddress,
};

enum class CoordinateEntryResolveStatus : std::uint8_t {
    Resolved,
    ReadFailed,
    InvalidEntry,
    TargetOutsideExecutable,
    Loop,
    HopLimit,
};

struct CoordinateEntryBranchObservation {
    std::uint64_t address = 0;
    std::uint32_t instruction = 0;
};

struct CoordinateEntryBranchStep {
    std::uint64_t address = 0;
    std::uint64_t target = 0;
    std::uint32_t instruction = 0;
};

struct CoordinateEntryBranchResolution {
    CoordinateEntryResolveStatus status =
        CoordinateEntryResolveStatus::InvalidEntry;
    std::uint64_t rawEntry = 0;
    std::uint64_t resolvedEntry = 0;
    std::array<CoordinateEntryBranchStep,
               kCoordinateEntryMaximumDirectBranchHops>
        steps{};
    std::array<CoordinateEntryBranchObservation,
               kCoordinateEntryMaximumDirectBranchHops + 1>
        observations{};
    std::uint8_t hopCount = 0;
    std::uint8_t observationCount = 0;
};

constexpr CoordinateEntryBranchDecodeStatus
DecodeArm64UnconditionalImmediateBranch(
    std::uint64_t address,
    std::uint32_t instruction,
    std::uint64_t& target) noexcept {
    constexpr std::uint32_t kOpcodeMask = UINT32_C(0xFC000000);
    constexpr std::uint32_t kOpcode = UINT32_C(0x14000000);
    constexpr std::uint32_t kImmediateMask = UINT32_C(0x03FFFFFF);
    constexpr std::uint32_t kImmediateSign = UINT32_C(0x02000000);
    constexpr std::int64_t kImmediateRange = INT64_C(1) << 26U;

    target = 0;
    if ((instruction & kOpcodeMask) != kOpcode) {
        return CoordinateEntryBranchDecodeStatus::NotBranch;
    }
    if ((address & 3U) != 0) {
        return CoordinateEntryBranchDecodeStatus::InvalidAddress;
    }

    const std::uint32_t encodedImmediate = instruction & kImmediateMask;
    std::int64_t immediate = static_cast<std::int64_t>(encodedImmediate);
    if ((encodedImmediate & kImmediateSign) != 0) {
        immediate -= kImmediateRange;
    }
    const std::int64_t displacement = immediate * 4;
    if (displacement >= 0) {
        const auto increment = static_cast<std::uint64_t>(displacement);
        if (address > std::numeric_limits<std::uint64_t>::max() - increment) {
            return CoordinateEntryBranchDecodeStatus::InvalidAddress;
        }
        target = address + increment;
    } else {
        const auto decrement =
            static_cast<std::uint64_t>(-(displacement + 1)) + 1;
        if (address < decrement) {
            return CoordinateEntryBranchDecodeStatus::InvalidAddress;
        }
        target = address - decrement;
    }
    return (target & 3U) == 0
        ? CoordinateEntryBranchDecodeStatus::Resolved
        : CoordinateEntryBranchDecodeStatus::InvalidAddress;
}

template <typename ReadInstruction, typename ContainsExecutableRange>
CoordinateEntryBranchResolution ResolveCoordinateEntryBranchChain(
    std::uint64_t entry,
    ReadInstruction&& readInstruction,
    ContainsExecutableRange&& containsExecutableRange) {
    CoordinateEntryBranchResolution result{};
    result.rawEntry = entry;
    result.resolvedEntry = entry;
    if ((entry & 3U) != 0 ||
        !containsExecutableRange(entry, sizeof(std::uint32_t))) {
        result.status = CoordinateEntryResolveStatus::InvalidEntry;
        return result;
    }

    std::array<std::uint64_t,
               kCoordinateEntryMaximumDirectBranchHops + 1>
        visited{};
    std::size_t visitedCount = 1;
    visited[0] = entry;
    std::uint64_t current = entry;

    for (std::size_t observationIndex = 0;
         observationIndex <= kCoordinateEntryMaximumDirectBranchHops;
         ++observationIndex) {
        std::uint32_t instruction = 0;
        if (!readInstruction(current, instruction)) {
            result.status = CoordinateEntryResolveStatus::ReadFailed;
            result.resolvedEntry = current;
            return result;
        }
        result.observations[result.observationCount++] = {
            current,
            instruction,
        };

        std::uint64_t target = 0;
        const CoordinateEntryBranchDecodeStatus decode =
            DecodeArm64UnconditionalImmediateBranch(
                current, instruction, target);
        if (decode == CoordinateEntryBranchDecodeStatus::NotBranch) {
            result.status = CoordinateEntryResolveStatus::Resolved;
            result.resolvedEntry = current;
            return result;
        }
        if (decode != CoordinateEntryBranchDecodeStatus::Resolved) {
            result.status = CoordinateEntryResolveStatus::InvalidEntry;
            result.resolvedEntry = current;
            return result;
        }
        if (observationIndex == kCoordinateEntryMaximumDirectBranchHops) {
            result.status = CoordinateEntryResolveStatus::HopLimit;
            result.resolvedEntry = current;
            return result;
        }
        if (!containsExecutableRange(target, sizeof(std::uint32_t))) {
            result.status =
                CoordinateEntryResolveStatus::TargetOutsideExecutable;
            result.resolvedEntry = current;
            return result;
        }
        for (std::size_t index = 0; index < visitedCount; ++index) {
            if (visited[index] == target) {
                result.status = CoordinateEntryResolveStatus::Loop;
                result.resolvedEntry = current;
                return result;
            }
        }

        result.steps[result.hopCount++] = {
            current,
            target,
            instruction,
        };
        visited[visitedCount++] = target;
        current = target;
    }

    result.status = CoordinateEntryResolveStatus::HopLimit;
    result.resolvedEntry = current;
    return result;
}

}  // namespace lengjing::game::native
