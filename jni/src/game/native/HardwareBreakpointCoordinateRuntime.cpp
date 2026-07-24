#if 0

#include "game/native/HardwareBreakpointCoordinateRuntime.h"

#include <cmath>
#include <limits>
#include <utility>

namespace lengjing::game::native {
namespace {

constexpr std::uintptr_t kCoordinateValueOffset = 0x80;
constexpr std::uintptr_t kCoordinateIdOffset = 0x2D8;
constexpr std::uintptr_t kPointerPayloadMask =
    UINT64_C(0x00FFFFFFFFFFFFFF);

static_assert(sizeof(HardwareBreakpointCoordinate) == 12);

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
    static_cast<void>(manager);
    return SampleRecordsBase();
}

bool HardwareBreakpointCoordinateRuntime::Lookup(
    std::uint32_t id,
    std::uintptr_t world,
    HardwareBreakpointCoordinate& coordinate) noexcept {
    return Lookup(id, 0, world, coordinate);
}

bool HardwareBreakpointCoordinateRuntime::Lookup(
    std::uint32_t id,
    std::uintptr_t mesh,
    std::uintptr_t world,
    HardwareBreakpointCoordinate& coordinate) noexcept {
    coordinate = {};
    if (!active_ || id == 0 || world == 0 || world != world_) {
        return false;
    }
    const auto found = coordinates_.find(id);
    if (found == coordinates_.end()) return false;
    const std::uintptr_t normalizedMesh = mesh & kPointerPayloadMask;
    if (normalizedMesh != 0 && found->second.mesh != normalizedMesh) {
        return false;
    }
    coordinate = found->second.value;
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

    if (lastHitAddress_ != 0 && lastHitAddress_ != hitAddress) {
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
                previous.pc == record.pc &&
                previous.x20 == record.x20 &&
                previous.x21 == record.x21) {
                seen = true;
                break;
            }
        }
        seenRecords_[index] = {
            record.tid,
            record.hitCount,
            record.pc,
            record.x20,
            record.x21,
            true,
        };
        if (seen) continue;

        const std::uintptr_t coordinateBase =
            record.x20 & kPointerPayloadMask;
        const std::uintptr_t mesh =
            record.x21 & kPointerPayloadMask;
        std::uintptr_t coordinateAddress = 0;
        std::uintptr_t idAddress = 0;
        if (!AddOffset(
                coordinateBase,
                kCoordinateValueOffset,
                coordinateAddress) ||
            !AddOffset(mesh, kCoordinateIdOffset, idAddress)) {
            continue;
        }

        std::uint32_t firstId = 0;
        std::uint32_t secondId = 0;
        HardwareBreakpointCoordinate coordinate{};
        if (!ReadMemory(
                callbacks_, idAddress, &firstId, sizeof(firstId)) ||
            firstId == 0 ||
            !ReadMemory(
                callbacks_,
                coordinateAddress,
                &coordinate,
                sizeof(coordinate)) ||
            !ReadMemory(
                callbacks_, idAddress, &secondId, sizeof(secondId)) ||
            firstId != secondId || !IsValidCoordinate(coordinate)) {
            continue;
        }

        const auto previousId = meshIds_.find(mesh);
        if (previousId != meshIds_.end() &&
            previousId->second != firstId) {
            const auto previousCoordinate =
                coordinates_.find(previousId->second);
            if (previousCoordinate != coordinates_.end() &&
                previousCoordinate->second.mesh == mesh) {
                coordinates_.erase(previousCoordinate);
            }
        }
        const auto previousMesh = coordinates_.find(firstId);
        if (previousMesh != coordinates_.end() &&
            previousMesh->second.mesh != mesh) {
            meshIds_.erase(previousMesh->second.mesh);
        }
        meshIds_[mesh] = firstId;
        coordinates_[firstId] = PublishedCoordinate{mesh, coordinate};
        recordsBase_ = coordinateBase;
        ++acceptedSampleCount_;
    }
    return true;
}

void HardwareBreakpointCoordinateRuntime::ClearWorldState() noexcept {
    seenRecords_.fill({});
    coordinates_.clear();
    meshIds_.clear();
    recordsBase_ = 0;
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
