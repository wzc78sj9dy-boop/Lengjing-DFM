#pragma once

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

struct CoordinateExecutionProfileOffsets {
    std::uint64_t rootOffset = 0;
    std::uint64_t pointerOffset = 0;
    std::uint64_t entryOffset = 0;
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
inline constexpr std::uint64_t kCoordinateExecutionPointerMask =
    UINT64_C(0x00FFFFFFFFFFFFFF);
inline constexpr std::uint64_t kCoordinateExecutionPointerMin =
    UINT64_C(0x0000006000000000);
inline constexpr std::uint64_t kCoordinateExecutionPointerMax =
    UINT64_C(0x0000008000000000);
inline constexpr std::uint64_t kCoordinateExecutionReturnStubMagic =
    UINT64_C(0xD61F020058000050);

struct CoordinateExecutionCandidateScanResult {
    std::vector<CoordinateExecutionCandidate> candidates;
    bool truncated = false;
};

CoordinateExecutionProfileOffsets GetCoordinateExecutionProfileOffsets(
    std::uint32_t scanProfile) noexcept;

CoordinateExecutionCandidateScanResult ScanCoordinateExecutionCandidates(
    const CoordinateExecutionReadCallback& read,
    const CoordinateExecutionModuleSnapshot& module,
    std::uint64_t scanAnchor,
    std::uint64_t relativeEntry);

CoordinateExecutionCandidateScanResult
DiscoverCoordinateExecutionCandidates(
    const CoordinateExecutionReadCallback& read,
    const CoordinateExecutionModuleSnapshot& module,
    std::uint64_t configuredModuleBase,
    std::uint32_t scanProfile);

bool ScanFirstCoordinateExecutionCandidate(
    const CoordinateExecutionReadCallback& read,
    const CoordinateExecutionModuleSnapshot& module,
    std::uint64_t scanAnchor,
    std::uint64_t relativeEntry,
    CoordinateExecutionCandidate* candidate);

bool DiscoverFirstCoordinateExecutionCandidate(
    const CoordinateExecutionReadCallback& read,
    const CoordinateExecutionModuleSnapshot& module,
    std::uint64_t configuredModuleBase,
    std::uint32_t scanProfile,
    CoordinateExecutionCandidate* candidate);

}  // namespace lengjing::game::native
