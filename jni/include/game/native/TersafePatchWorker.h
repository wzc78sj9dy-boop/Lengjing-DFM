#pragma once

#include "game/ProjectileTrackingFeature.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#if LENGJING_ENABLE_PROJECTILE_TRACKING
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <sys/types.h>
#endif

namespace lengjing::game::native {

namespace tersafe_patch_layout {

inline constexpr std::uintptr_t kRootOffset = 0x566810;
inline constexpr std::array<std::uintptr_t, 3> kCandidateOffsets{
    0x2B0,
    0x2B8,
    0x2C0,
};
inline constexpr std::uintptr_t kGroupOffset = 0xE8;
inline constexpr std::array<std::uintptr_t, 3> kTargetOffsets{
    0x00,
    0x08,
    0x10,
};
inline constexpr std::uint32_t kCandidateTag = 0x50;
inline constexpr std::uint64_t kPointerMask = 0x00FFFFFFFFFFFFFFULL;
inline constexpr std::uint8_t kPatchValue = 0x06;

}  // namespace tersafe_patch_layout

namespace detail {

inline bool AddPatchOffset(std::uintptr_t base,
                           std::uintptr_t offset,
                           std::uintptr_t& result) noexcept {
    if (base == 0 ||
        offset > std::numeric_limits<std::uintptr_t>::max() - base) {
        result = 0;
        return false;
    }
    result = base + offset;
    return true;
}

template <typename Reader, typename Value>
bool ReadPatchValue(Reader& reader,
                    std::uintptr_t address,
                    Value& value) {
    value = {};
    return address != 0 && reader(address, &value, sizeof(value));
}

inline bool NormalizePatchPointer(std::uint64_t raw,
                                  std::uintptr_t& result) noexcept {
    raw &= tersafe_patch_layout::kPointerMask;
    if (raw == 0 ||
        raw > static_cast<std::uint64_t>(
            std::numeric_limits<std::uintptr_t>::max())) {
        result = 0;
        return false;
    }
    result = static_cast<std::uintptr_t>(raw);
    return true;
}

}  // namespace detail

template <typename Reader>
bool ResolveTersafePatchTargets(
    std::uintptr_t moduleBase,
    Reader&& remoteRead,
    std::array<std::uintptr_t, 3>& targets) {
    targets.fill(0);

    std::uintptr_t rootAddress = 0;
    std::uint64_t rawRoot = 0;
    std::uintptr_t root = 0;
    if (!detail::AddPatchOffset(
            moduleBase,
            tersafe_patch_layout::kRootOffset,
            rootAddress) ||
        !detail::ReadPatchValue(remoteRead, rootAddress, rawRoot) ||
        !detail::NormalizePatchPointer(rawRoot, root)) {
        return false;
    }

    std::uintptr_t candidate = 0;
    for (const std::uintptr_t offset :
         tersafe_patch_layout::kCandidateOffsets) {
        std::uintptr_t pointerAddress = 0;
        std::uint64_t rawCandidate = 0;
        std::uint32_t tag = 0;
        if (!detail::AddPatchOffset(root, offset, pointerAddress) ||
            !detail::ReadPatchValue(
                remoteRead, pointerAddress, rawCandidate)) {
            continue;
        }
        std::uintptr_t current = 0;
        if (!detail::NormalizePatchPointer(rawCandidate, current)) continue;
        if (detail::ReadPatchValue(remoteRead, current, tag) &&
            tag == tersafe_patch_layout::kCandidateTag) {
            candidate = current;
            break;
        }
    }
    if (candidate == 0) return false;

    std::uintptr_t groupAddress = 0;
    std::uint64_t rawGroup = 0;
    std::uintptr_t group = 0;
    if (!detail::AddPatchOffset(
            candidate,
            tersafe_patch_layout::kGroupOffset,
            groupAddress) ||
        !detail::ReadPatchValue(remoteRead, groupAddress, rawGroup) ||
        !detail::NormalizePatchPointer(rawGroup, group)) {
        return false;
    }

    for (std::size_t index = 0; index < targets.size(); ++index) {
        std::uintptr_t targetAddress = 0;
        std::uint64_t rawTarget = 0;
        if (!detail::AddPatchOffset(
                group,
                tersafe_patch_layout::kTargetOffsets[index],
                targetAddress) ||
            !detail::ReadPatchValue(
                remoteRead, targetAddress, rawTarget) ||
            !detail::NormalizePatchPointer(rawTarget, targets[index])) {
            targets.fill(0);
            return false;
        }
    }
    return true;
}

#if LENGJING_ENABLE_PROJECTILE_TRACKING

class MemoryTransport;

class TersafePatchWorker final {
public:
    TersafePatchWorker() = default;
    ~TersafePatchWorker();

    TersafePatchWorker(const TersafePatchWorker&) = delete;
    TersafePatchWorker& operator=(const TersafePatchWorker&) = delete;

    bool Start(MemoryTransport& memory, pid_t processId) noexcept;
    void Stop() noexcept;

private:
    void Run() noexcept;
    bool PatchOnce();
    bool WaitForStop(std::chrono::milliseconds delay) noexcept;

    MemoryTransport* memory_ = nullptr;
    pid_t processId_ = -1;
    std::thread worker_;
    std::mutex stateMutex_;
    std::condition_variable wake_;
    bool stopRequested_ = false;
};

#endif

}  // namespace lengjing::game::native
