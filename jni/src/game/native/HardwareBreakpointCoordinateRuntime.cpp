#if 0

#include "game/native/HardwareBreakpointCoordinateRuntime.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace lengjing::game::native {
namespace {

constexpr std::uintptr_t kManagerOffset = 0x1B8;
constexpr std::uintptr_t kIdArrayOffset = 0xF98;
constexpr std::uintptr_t kCountOffset = 0xFA0;
constexpr std::size_t kCoordinateRecordStride = 0x40;
constexpr std::size_t kCoordinateXOffset = 0x30;
constexpr std::size_t kCoordinateYOffset = 0x34;
constexpr std::size_t kCoordinateZOffset = 0x38;
constexpr std::int32_t kMinimumCoordinateCount = 15;
constexpr std::int32_t kMaximumCoordinateCount = 16384;
constexpr float kCoordinateZAdjustment = 80.0f;
constexpr std::uintptr_t kPointerPayloadMask =
    UINT64_C(0x00FFFFFFFFFFFFFF);
constexpr std::uintptr_t kRejectedCandidateMask = UINT64_C(0xFFFF0000);
constexpr std::uintptr_t kRejectedCandidateValue = UINT64_C(0xDEAD0000);

static_assert(sizeof(HardwareBreakpointCoordinate) == 12);
static_assert(kCoordinateZOffset + sizeof(float) <=
              kCoordinateRecordStride);

bool AddOffset(std::uintptr_t base,
               std::uintptr_t offset,
               std::uintptr_t& address) noexcept {
    if (base == 0 ||
        base > std::numeric_limits<std::uintptr_t>::max() - offset) {
        return false;
    }
    address = base + offset;
    return true;
}

bool IsReadableRange(std::uintptr_t address, std::size_t size) noexcept {
    return address != 0 && size != 0 &&
        size - 1 <=
            std::numeric_limits<std::uintptr_t>::max() - address;
}

bool ReadMemory(const HardwareBreakpointCoordinateCallbacks& callbacks,
                std::uintptr_t address,
                void* destination,
                std::size_t size) noexcept {
    if (destination == nullptr || !IsReadableRange(address, size)) {
        return false;
    }
    try {
        return callbacks.readMemory(address, destination, size);
    } catch (...) {
        return false;
    }
}

bool IsValidCoordinate(
    const HardwareBreakpointCoordinate& coordinate) noexcept {
    return std::isfinite(coordinate.x) &&
        std::isfinite(coordinate.y) &&
        std::isfinite(coordinate.z) &&
        (coordinate.x != 0.0f ||
         coordinate.y != 0.0f ||
         coordinate.z != 0.0f);
}

std::uintptr_t MostFrequentCandidate(
    const std::array<std::uintptr_t, 10>& candidates,
    std::size_t count) noexcept {
    if (count == 0 || count > candidates.size()) return 0;

    std::uintptr_t best = 0;
    std::size_t bestCount = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const std::uintptr_t candidate = candidates[index];
        if (candidate == 0) continue;
        std::size_t occurrences = 0;
        for (std::size_t other = 0; other < count; ++other) {
            if (candidates[other] == candidate) ++occurrences;
        }
        if (occurrences > bestCount) {
            best = candidate;
            bestCount = occurrences;
        }
    }
    return best;
}

}  // namespace

HardwareBreakpointCoordinateRuntime::~HardwareBreakpointCoordinateRuntime() {
    static_cast<void>(Stop());
}

bool HardwareBreakpointCoordinateRuntime::Start(
    std::uintptr_t breakpointAddress,
    HardwareBreakpointCoordinateCallbacks callbacks) noexcept {
    if (breakpointAddress == 0 || (breakpointAddress & 3U) != 0 ||
        !callbacks) {
        return false;
    }
    if (active_ && breakpointAddress_ == breakpointAddress) return true;
    if (active_ && !Stop()) return false;

    bool configured = false;
    try {
        configured = callbacks.configureBreakpoint(breakpointAddress);
    } catch (...) {
        configured = false;
    }
    if (!configured) {
        try {
            static_cast<void>(callbacks.removeBreakpoints());
        } catch (...) {
        }
        return false;
    }

    callbacks_ = std::move(callbacks);
    breakpointAddress_ = breakpointAddress;
    active_ = true;
    ClearSamplingState();
    return true;
}

bool HardwareBreakpointCoordinateRuntime::Stop() noexcept {
    if (!active_) {
        callbacks_ = {};
        breakpointAddress_ = 0;
        ClearSamplingState();
        return true;
    }

    bool removed = false;
    try {
        removed = callbacks_.removeBreakpoints();
    } catch (...) {
        removed = false;
    }
    if (!removed) return false;

    active_ = false;
    callbacks_ = {};
    breakpointAddress_ = 0;
    ClearSamplingState();
    return true;
}

bool HardwareBreakpointCoordinateRuntime::Poll(
    std::uintptr_t world) noexcept {
    return Poll(world, 0);
}

bool HardwareBreakpointCoordinateRuntime::Poll(
    std::uintptr_t world,
    std::uintptr_t manager) noexcept {
    if (!active_ || world == 0) return false;
    ++pollCount_;
    ResetWorld(world);

    if (!SampleRecordsBase() || recordsBase_ == 0) return false;

    if (manager == 0) {
        std::uintptr_t managerAddress = 0;
        if (!AddOffset(world, kManagerOffset, managerAddress) ||
            !ReadMemory(
                callbacks_, managerAddress, &manager, sizeof(manager))) {
            return false;
        }
    }
    if (manager == 0) return false;
    return RefreshCoordinateTable(world, manager);
}

bool HardwareBreakpointCoordinateRuntime::Lookup(
    std::uint32_t id,
    std::uintptr_t world,
    HardwareBreakpointCoordinate& coordinate) noexcept {
    coordinate = {};
    if (!active_ || id == 0 || world == 0 || world != world_) {
        return false;
    }
    const auto found = coordinates_.find(id);
    if (found == coordinates_.end()) return false;
    coordinate = found->second;
    return true;
}

void HardwareBreakpointCoordinateRuntime::ResetWorld(
    std::uintptr_t world) noexcept {
    if (world_ == world) return;
    ClearWorldState();
    world_ = world;
}

bool HardwareBreakpointCoordinateRuntime::IsActive() const noexcept {
    return active_;
}

std::uintptr_t
HardwareBreakpointCoordinateRuntime::BreakpointAddress() const noexcept {
    return breakpointAddress_;
}

std::uintptr_t
HardwareBreakpointCoordinateRuntime::RecordsBase() const noexcept {
    return recordsBase_;
}

std::size_t
HardwareBreakpointCoordinateRuntime::PublishedCoordinateCount() const noexcept {
    return coordinates_.size();
}

std::uint64_t HardwareBreakpointCoordinateRuntime::PollCount() const noexcept {
    return pollCount_;
}

std::uint64_t
HardwareBreakpointCoordinateRuntime::AcceptedSampleCount() const noexcept {
    return acceptedSampleCount_;
}

bool HardwareBreakpointCoordinateRuntime::SampleRecordsBase() noexcept {
    std::size_t recordsRead = 0;
    std::size_t totalRecords = 0;
    std::uintptr_t hitAddress = 0;
    bool read = false;
    try {
        read = callbacks_.readRecords(
            records_.data(), records_.size(), recordsRead, hitAddress,
            totalRecords);
    } catch (...) {
        read = false;
    }
    if (!read || recordsRead > records_.size() ||
        totalRecords > records_.size() ||
        hitAddress != breakpointAddress_) {
        return false;
    }

    if ((lastHitAddress_ != 0 && lastHitAddress_ != hitAddress) ||
        totalRecords < lastTotalRecords_) {
        ClearWorldState();
    }
    lastHitAddress_ = hitAddress;
    lastTotalRecords_ = totalRecords;

    for (std::size_t index = recordsRead;
         index < seenRecords_.size();
         ++index) {
        seenRecords_[index] = {};
    }

    for (std::size_t index = 0; index < recordsRead; ++index) {
        const ExecutionBreakpointRecord& record = records_[index];
        if (record.tid <= 0 || record.hitCount == 0 ||
            record.pc != breakpointAddress_) {
            continue;
        }

        bool seen = false;
        for (const SeenRecord& previous : seenRecords_) {
            if (previous.valid && previous.tid == record.tid &&
                previous.hitCount == record.hitCount &&
                previous.pc == record.pc) {
                seen = true;
                break;
            }
        }
        seenRecords_[index] = {
            record.tid,
            record.hitCount,
            record.pc,
            true,
        };
        if (seen) continue;

        const std::uintptr_t candidate =
            record.x23 & kPointerPayloadMask;
        std::uint64_t probe = 0;
        if (candidate == 0 ||
            (candidate & kRejectedCandidateMask) ==
                kRejectedCandidateValue ||
            !ReadMemory(
                callbacks_, candidate, &probe, sizeof(probe))) {
            continue;
        }
        candidateRing_[candidateWriteIndex_] = candidate;
        candidateWriteIndex_ =
            (candidateWriteIndex_ + 1) % candidateRing_.size();
        if (candidateCount_ < candidateRing_.size()) {
            ++candidateCount_;
        }
        ++acceptedSampleCount_;
    }

    const std::uintptr_t selected =
        MostFrequentCandidate(candidateRing_, candidateCount_);
    if (selected != recordsBase_) {
        recordsBase_ = selected;
        coordinates_.clear();
    }
    return true;
}

bool HardwareBreakpointCoordinateRuntime::RefreshCoordinateTable(
    std::uintptr_t world,
    std::uintptr_t manager) noexcept {
    std::uintptr_t idArrayAddress = 0;
    std::uintptr_t countAddress = 0;
    if (!AddOffset(manager, kIdArrayOffset, idArrayAddress) ||
        !AddOffset(manager, kCountOffset, countAddress)) {
        return false;
    }

    std::uintptr_t idArray = 0;
    std::int32_t count = 0;
    if (!ReadMemory(
            callbacks_, idArrayAddress, &idArray, sizeof(idArray)) ||
        !ReadMemory(callbacks_, countAddress, &count, sizeof(count)) ||
        idArray == 0 || count < kMinimumCoordinateCount ||
        count > kMaximumCoordinateCount) {
        return false;
    }

    const std::size_t itemCount = static_cast<std::size_t>(count);
    const std::size_t recordsSize =
        itemCount * kCoordinateRecordStride;
    const std::size_t idsSize = itemCount * sizeof(std::uint32_t);
    if (!IsReadableRange(recordsBase_, recordsSize) ||
        !IsReadableRange(idArray, idsSize)) {
        return false;
    }

    try {
        std::vector<std::uint8_t> records(recordsSize);
        std::vector<std::uint32_t> ids(itemCount);
        if (!ReadMemory(
                callbacks_, recordsBase_, records.data(), records.size()) ||
            !ReadMemory(callbacks_, idArray, ids.data(), idsSize)) {
            return false;
        }

        std::uintptr_t managerAddress = 0;
        std::uintptr_t verifiedManager = 0;
        std::uintptr_t verifiedIdArray = 0;
        std::int32_t verifiedCount = 0;
        if (!AddOffset(world, kManagerOffset, managerAddress) ||
            !ReadMemory(
                callbacks_, managerAddress, &verifiedManager,
                sizeof(verifiedManager)) ||
            !ReadMemory(
                callbacks_, idArrayAddress, &verifiedIdArray,
                sizeof(verifiedIdArray)) ||
            !ReadMemory(
                callbacks_, countAddress, &verifiedCount,
                sizeof(verifiedCount)) ||
            verifiedManager != manager ||
            verifiedIdArray != idArray ||
            verifiedCount != count) {
            return false;
        }

        std::unordered_map<std::uint32_t, HardwareBreakpointCoordinate>
            next;
        next.reserve(itemCount);
        for (std::size_t index = 0; index < itemCount; ++index) {
            const std::uint32_t id = ids[index];
            if (id == 0) continue;

            const std::size_t recordOffset =
                index * kCoordinateRecordStride;
            HardwareBreakpointCoordinate coordinate{};
            std::memcpy(
                &coordinate.x,
                records.data() + recordOffset + kCoordinateXOffset,
                sizeof(coordinate.x));
            std::memcpy(
                &coordinate.y,
                records.data() + recordOffset + kCoordinateYOffset,
                sizeof(coordinate.y));
            std::memcpy(
                &coordinate.z,
                records.data() + recordOffset + kCoordinateZOffset,
                sizeof(coordinate.z));
            if (!IsValidCoordinate(coordinate)) continue;
            coordinate.z += kCoordinateZAdjustment;
            if (!std::isfinite(coordinate.z)) continue;
            next[id] = coordinate;
        }

        if (world != world_) return false;
        coordinates_.swap(next);
        return true;
    } catch (...) {
        return false;
    }
}

void HardwareBreakpointCoordinateRuntime::ClearWorldState() noexcept {
    seenRecords_.fill({});
    candidateRing_.fill(0);
    coordinates_.clear();
    recordsBase_ = 0;
    candidateWriteIndex_ = 0;
    candidateCount_ = 0;
    lastTotalRecords_ = 0;
    lastHitAddress_ = 0;
}

void HardwareBreakpointCoordinateRuntime::ClearSamplingState() noexcept {
    records_.fill({});
    ClearWorldState();
    world_ = 0;
    pollCount_ = 0;
    acceptedSampleCount_ = 0;
}

}  // namespace lengjing::game::native

#endif
