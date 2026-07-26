#pragma once

#include "game/native/CoordinateExecutionLayout.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace lengjing::game::native {

using CoordinateExecutionReadCallback = std::function<bool(
    std::uint64_t address,
    void* output,
    std::size_t size)>;

struct CoordinateExecutionModuleSnapshot {
    std::uint64_t guestBase = 0;
    const std::byte* hostBytes = nullptr;
    std::size_t size = 0;
};

struct CoordinateExecutionCandidate {
    std::uint64_t q0 = 0;
    std::uint64_t q1 = 0;
    std::uint64_t q2 = 0;
    std::uint64_t q3 = 0;

    friend constexpr bool operator==(
        const CoordinateExecutionCandidate& left,
        const CoordinateExecutionCandidate& right) noexcept {
        return left.q0 == right.q0 && left.q1 == right.q1 &&
            left.q2 == right.q2 && left.q3 == right.q3;
    }
};

static_assert(sizeof(CoordinateExecutionCandidate) == 32);

inline constexpr std::size_t kCoordinateExecutionCandidateLimit = 256;
inline constexpr std::uint64_t kCoordinateExecutionMinimumCodeRangeSize =
    UINT64_C(0x80000);
inline constexpr std::uint64_t kCoordinateExecutionPointerMask =
    UINT64_C(0x00FFFFFFFFFFFFFF);
inline constexpr std::uint64_t kCoordinateExecutionPointerMin =
    UINT64_C(0x0000006000000000);
inline constexpr std::uint64_t kCoordinateExecutionPointerMax =
    UINT64_C(0x0000008000000000);
struct CoordinateExecutionCodeRange {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;

    constexpr bool IsValid() const noexcept {
        return begin != 0 && end > begin &&
            end - begin >= kCoordinateExecutionMinimumCodeRangeSize;
    }

    constexpr std::uint64_t Size() const noexcept {
        return IsValid() ? end - begin : 0;
    }
};

struct CoordinateExecutionCandidateScanResult {
    std::vector<CoordinateExecutionCandidate> candidates;
    bool truncated = false;
};

struct CoordinateExecutionDiscoveryInput {
    std::uint64_t scanAnchor = 0;
    std::uint64_t root = 0;
    std::uint64_t rawEntry = 0;
};

bool ResolveCoordinateExecutionDiscoveryInput(
    const CoordinateExecutionReadCallback& read,
    std::uint64_t configuredModuleBase,
    const CoordinateExecutionLayout& layout,
    CoordinateExecutionDiscoveryInput* input);

bool IsCoordinateExecutionCodeRangeCompatible(
    const CoordinateExecutionCodeRange& range,
    std::uint64_t root,
    std::uint64_t rawEntry) noexcept;

bool ResolveCoordinateExecutionCodeAddress(
    const CoordinateExecutionCodeRange& range,
    std::uint64_t rawEntry,
    std::uint64_t* address) noexcept;

bool SelectCoordinateExecutionCodeRange(
    const std::vector<CoordinateExecutionCodeRange>& ranges,
    std::uint64_t root,
    std::uint64_t rawEntry,
    CoordinateExecutionCodeRange* selected) noexcept;

CoordinateExecutionCandidateScanResult ScanCoordinateExecutionCandidates(
    const CoordinateExecutionReadCallback& read,
    const CoordinateExecutionModuleSnapshot& module,
    const CoordinateExecutionModuleSnapshot& code,
    std::uint64_t scanAnchor,
    std::uint64_t relativeEntry,
    const CoordinateExecutionLayout& layout);

CoordinateExecutionCandidateScanResult
DiscoverCoordinateExecutionCandidates(
    const CoordinateExecutionReadCallback& read,
    const CoordinateExecutionModuleSnapshot& module,
    const CoordinateExecutionModuleSnapshot& code,
    std::uint64_t configuredModuleBase,
    const CoordinateExecutionLayout& layout);

bool ScanFirstCoordinateExecutionCandidate(
    const CoordinateExecutionReadCallback& read,
    const CoordinateExecutionModuleSnapshot& module,
    const CoordinateExecutionModuleSnapshot& code,
    std::uint64_t scanAnchor,
    std::uint64_t relativeEntry,
    const CoordinateExecutionLayout& layout,
    CoordinateExecutionCandidate* candidate);

bool DiscoverFirstCoordinateExecutionCandidate(
    const CoordinateExecutionReadCallback& read,
    const CoordinateExecutionModuleSnapshot& module,
    const CoordinateExecutionModuleSnapshot& code,
    std::uint64_t configuredModuleBase,
    const CoordinateExecutionLayout& layout,
    CoordinateExecutionCandidate* candidate);

}  // namespace lengjing::game::native
