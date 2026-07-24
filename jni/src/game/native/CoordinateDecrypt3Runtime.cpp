#if 0

#include "game/native/CoordinateDecrypt3Runtime.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__ANDROID__) || defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace lengjing::game::native {
namespace {

constexpr char kCarrierPath[] =
    "/sys/fs/cgroup/cgroup.controllers";
constexpr std::uintptr_t kWorldManagerOffset = 0x1b8;
constexpr std::uintptr_t kIdArrayOffset = 0xf98;
constexpr std::uintptr_t kRecordCountOffset = 0xfa0;
constexpr std::uintptr_t kMeshIdOffset = 0x2d8;
constexpr std::size_t kRecordStride = 0x40;
constexpr std::size_t kPositionOffset = 0x30;
constexpr std::uint32_t kMinimumRecordCount = 15;
constexpr std::uint32_t kMaximumRecordCount = 16384;
constexpr float kOutputHeight = 80.0f;
constexpr std::size_t kSampleCapacity = 10;

struct MetadataSnapshot {
    std::uintptr_t idArray = 0;
    std::uint32_t count = 0;
    std::uint32_t capacity = 0;
};

struct PackedRecord {
    std::array<std::uint8_t, kPositionOffset> prefix{};
    CoordinateDecrypt3Position position{};
    std::array<std::uint8_t,
               kRecordStride - kPositionOffset -
                   sizeof(CoordinateDecrypt3Position)>
        suffix{};
};

static_assert(sizeof(PackedRecord) == kRecordStride);
static_assert(offsetof(PackedRecord, position) == kPositionOffset);

bool AddOffset(std::uintptr_t base,
               std::uintptr_t offset,
               std::uintptr_t& result) noexcept {
    if (base == 0 ||
        offset > std::numeric_limits<std::uintptr_t>::max() - base) {
        return false;
    }
    result = base + offset;
    return true;
}

bool IsFinitePosition(
    const CoordinateDecrypt3Position& position) noexcept {
    return std::isfinite(position.x) &&
        std::isfinite(position.y) &&
        std::isfinite(position.z) &&
        (position.x != 0.0f ||
         position.y != 0.0f ||
         position.z != 0.0f);
}

bool IsValidMetadata(const MetadataSnapshot& metadata) noexcept {
    return metadata.idArray != 0 &&
        metadata.count >= kMinimumRecordCount &&
        metadata.count <= kMaximumRecordCount &&
        metadata.capacity >= metadata.count;
}

bool SameMetadata(const MetadataSnapshot& left,
                  const MetadataSnapshot& right) noexcept {
    return left.idArray == right.idArray &&
        left.count == right.count &&
        left.capacity == right.capacity;
}

bool SameExecutionRecord(const ExecutionBreakpointRecord& left,
                         const ExecutionBreakpointRecord& right) noexcept {
    return left.tid == right.tid &&
        left.hitCount == right.hitCount &&
        left.pc == right.pc &&
        left.x23 == right.x23;
}

}  // namespace

struct CoordinateDecrypt3Runtime::Impl {
    MemoryTransport* memory = nullptr;
    pid_t processId = -1;
    int carrierDescriptor = -1;
    CoordinateDecrypt3Backend backend =
        CoordinateDecrypt3Backend::None;
    CoordinateDecrypt3Probe probe{};
    std::uintptr_t world = 0;
    std::array<std::uintptr_t, kSampleCapacity> samples{};
    std::size_t sampleWriteIndex = 0;
    std::size_t sampleCount = 0;
    std::array<ExecutionBreakpointRecord,
               kExecutionBreakpointRecordLimit>
        executionRecords{};
    std::array<ExecutionBreakpointRecord,
               kExecutionBreakpointRecordLimit>
        seenExecutionRecords{};
    std::vector<std::uint32_t> ids;
    std::vector<PackedRecord> records;
    std::unordered_map<std::uint32_t, CoordinateDecrypt3Position>
        positions;

    ~Impl() {
        static_cast<void>(Stop());
    }

    void SetError(CoordinateDecrypt3Error error,
                  int systemError = 0) noexcept {
        probe.error = error;
        probe.systemError = systemError;
    }

    int Submit(std::uint32_t operation, void* payload) noexcept {
#if defined(__ANDROID__) || defined(__linux__)
        if (carrierDescriptor < 0 || operation == 0 ||
            payload == nullptr) {
            return -EINVAL;
        }
        coordinate_decrypt3_abi::Envelope envelope{
            operation,
            0,
            payload,
        };
        errno = 0;
        const ssize_t result = static_cast<ssize_t>(syscall(
            SYS_write,
            carrierDescriptor,
            &envelope,
            coordinate_decrypt3_abi::kRequestCount));
        if (result == 0) return 0;
        if (result < 0) return errno != 0 ? -errno : -EIO;
        return -EPROTO;
#else
        static_cast<void>(operation);
        static_cast<void>(payload);
        return -ENOSYS;
#endif
    }

    bool OpenPageTrap() noexcept {
#if defined(__ANDROID__) || defined(__linux__)
        carrierDescriptor =
            open(kCarrierPath, O_WRONLY | O_CLOEXEC);
        if (carrierDescriptor < 0) return false;

        std::int32_t target = processId;
        const int bindResult = Submit(
            coordinate_decrypt3_abi::kSetProcessOperation,
            &target);
        if (bindResult != 0) {
            close(carrierDescriptor);
            carrierDescriptor = -1;
            return false;
        }

        std::uint32_t reset = 0;
        static_cast<void>(Submit(
            coordinate_decrypt3_abi::kDisarmOperation,
            &reset));
        coordinate_decrypt3_abi::ArmPayload arm{};
        arm.processId = processId;
        arm.instructionAddress = probe.instructionAddress;
        const int armResult = Submit(
            coordinate_decrypt3_abi::kArmOperation,
            &arm);
        if (armResult != 0) {
            SetError(CoordinateDecrypt3Error::ArmFailed, armResult);
            close(carrierDescriptor);
            carrierDescriptor = -1;
            return false;
        }
        backend = CoordinateDecrypt3Backend::PageTrap;
        probe.backend = backend;
        return true;
#else
        return false;
#endif
    }

    bool OpenPageExecution() noexcept {
        if (memory == nullptr ||
            !memory->SupportsPageExecutionBreakpoints() ||
            !memory->ConfigurePageExecutionBreakpoint(
                probe.instructionAddress)) {
            return false;
        }
        backend = CoordinateDecrypt3Backend::PageExecution;
        probe.backend = backend;
        return true;
    }

    bool Start(MemoryTransport& transport,
               pid_t targetProcessId,
               std::uintptr_t instructionAddress) noexcept {
        if (targetProcessId <= 0 ||
            instructionAddress == 0 ||
            (instructionAddress & 3U) != 0 ||
            !transport.IsOpen()) {
            SetError(CoordinateDecrypt3Error::InvalidInput, -EINVAL);
            return false;
        }
        if (probe.active &&
            memory == &transport &&
            processId == targetProcessId &&
            probe.instructionAddress == instructionAddress) {
            return true;
        }
        static_cast<void>(Stop());

        memory = &transport;
        processId = targetProcessId;
        probe = {};
        probe.instructionAddress = instructionAddress;
        if (!OpenPageTrap() && !OpenPageExecution()) {
            SetError(CoordinateDecrypt3Error::EndpointUnavailable);
            memory = nullptr;
            processId = -1;
            return false;
        }
        probe.active = true;
        probe.error = CoordinateDecrypt3Error::SnapshotInvalid;
        return true;
    }

    void ClearPublishedState() noexcept {
        samples.fill(0);
        sampleWriteIndex = 0;
        sampleCount = 0;
        executionRecords.fill({});
        seenExecutionRecords.fill({});
        ids.clear();
        records.clear();
        positions.clear();
        probe.recordsBase = 0;
        probe.manager = 0;
        probe.idArray = 0;
        probe.recordCount = 0;
        probe.sampleCount = 0;
        probe.publishedCount = 0;
        probe.sequence = 0;
    }

    bool ReArmPageTrap() noexcept {
        if (backend != CoordinateDecrypt3Backend::PageTrap) return true;
        std::uint32_t reset = 0;
        if (Submit(
                coordinate_decrypt3_abi::kDisarmOperation,
                &reset) != 0) {
            return false;
        }
        coordinate_decrypt3_abi::ArmPayload arm{};
        arm.processId = processId;
        arm.instructionAddress = probe.instructionAddress;
        return Submit(
                   coordinate_decrypt3_abi::kArmOperation,
                   &arm) == 0;
    }

    bool ValidateCandidate(std::uintptr_t candidate) noexcept {
        if (memory == nullptr || candidate == 0) return false;
        if ((candidate & UINT64_C(0xffff0000)) ==
            UINT64_C(0xdead0000)) {
            return false;
        }
        std::uint64_t probeValue = 0;
        return memory->Read(
            candidate, &probeValue, sizeof(probeValue));
    }

    void PublishSample(std::uintptr_t candidate) noexcept {
        samples[sampleWriteIndex] = candidate;
        sampleWriteIndex =
            (sampleWriteIndex + 1) % samples.size();
        if (sampleCount < samples.size()) ++sampleCount;
        probe.sampleCount = sampleCount;
        probe.recordsBase =
            coordinate_decrypt3_abi::SelectSampleMode(
                samples, sampleCount);
        ++probe.acceptedSampleCount;
    }

    bool PollPageTrap() noexcept {
        coordinate_decrypt3_abi::PollPayload snapshot{};
        const int result = Submit(
            coordinate_decrypt3_abi::kPollOperation,
            &snapshot);
        if (result != 0) {
            SetError(CoordinateDecrypt3Error::PollFailed, result);
            return false;
        }
        std::uintptr_t candidate = 0;
        if (!coordinate_decrypt3_abi::SelectSnapshotCandidate(
                snapshot,
                probe.instructionAddress,
                probe.sequence,
                candidate)) {
            SetError(CoordinateDecrypt3Error::SnapshotInvalid);
            return false;
        }
        probe.sequence = snapshot.sequence;
        if (!ValidateCandidate(candidate)) {
            SetError(CoordinateDecrypt3Error::CandidateUnreadable);
            return false;
        }
        PublishSample(candidate);
        return true;
    }

    bool PollPageExecution() noexcept {
        if (memory == nullptr) return false;
        std::size_t recordsRead = 0;
        std::uintptr_t hitAddress = 0;
        std::size_t totalRecords = 0;
        if (!memory->ReadPageExecutionBreakpointRecords(
                executionRecords.data(),
                executionRecords.size(),
                recordsRead,
                hitAddress,
                totalRecords) ||
            hitAddress != probe.instructionAddress ||
            recordsRead > executionRecords.size() ||
            totalRecords > executionRecords.size()) {
            SetError(CoordinateDecrypt3Error::PollFailed);
            return false;
        }

        bool accepted = false;
        for (std::size_t index = 0; index < recordsRead; ++index) {
            const ExecutionBreakpointRecord& record =
                executionRecords[index];
            if (record.tid <= 0 ||
                record.hitCount == 0 ||
                record.pc != probe.instructionAddress) {
                continue;
            }
            bool seen = false;
            for (const ExecutionBreakpointRecord& previous :
                 seenExecutionRecords) {
                if (SameExecutionRecord(previous, record)) {
                    seen = true;
                    break;
                }
            }
            seenExecutionRecords[index] = record;
            if (seen) continue;

            const std::uintptr_t candidate =
                coordinate_decrypt3_abi::NormalizePointer(record.x23);
            if (!ValidateCandidate(candidate)) continue;
            PublishSample(candidate);
            accepted = true;
        }
        for (std::size_t index = recordsRead;
             index < seenExecutionRecords.size();
             ++index) {
            seenExecutionRecords[index] = {};
        }
        if (!accepted) {
            SetError(CoordinateDecrypt3Error::SnapshotInvalid);
        }
        return accepted;
    }

    bool ReadMetadata(std::uintptr_t manager,
                      MetadataSnapshot& metadata) noexcept {
        metadata = {};
        std::uintptr_t address = 0;
        return memory != nullptr &&
            AddOffset(manager, kIdArrayOffset, address) &&
            memory->Read(address, &metadata, sizeof(metadata));
    }

    bool PublishCoordinates() noexcept {
        if (memory == nullptr ||
            probe.recordsBase == 0 ||
            probe.manager == 0) {
            return false;
        }

        MetadataSnapshot before{};
        if (!ReadMetadata(probe.manager, before) ||
            !IsValidMetadata(before)) {
            SetError(CoordinateDecrypt3Error::MetadataInvalid);
            return false;
        }

        try {
            ids.resize(before.count);
            records.resize(before.count);
            const std::array<MemoryReadRequest, 2> requests{{
                {
                    before.idArray,
                    ids.data(),
                    ids.size() * sizeof(ids[0]),
                },
                {
                    probe.recordsBase,
                    records.data(),
                    records.size() * sizeof(records[0]),
                },
            }};
            std::array<std::uint8_t, 2> status{};
            if (memory->ReadBatch(
                    requests.data(),
                    requests.size(),
                    status.data()) != requests.size() ||
                status[0] == 0 ||
                status[1] == 0) {
                SetError(CoordinateDecrypt3Error::ArrayReadFailed);
                return false;
            }

            MetadataSnapshot after{};
            if (!ReadMetadata(probe.manager, after) ||
                !SameMetadata(before, after)) {
                SetError(CoordinateDecrypt3Error::ArrayChanged);
                return false;
            }

            positions.clear();
            positions.reserve(before.count);
            for (std::size_t index = 0;
                 index < before.count;
                 ++index) {
                const std::uint32_t id = ids[index];
                CoordinateDecrypt3Position position =
                    records[index].position;
                if (id == 0 || !IsFinitePosition(position)) continue;
                position.z += kOutputHeight;
                positions[id] = position;
            }
        } catch (const std::bad_alloc&) {
            SetError(CoordinateDecrypt3Error::ArrayReadFailed, -ENOMEM);
            return false;
        }

        probe.idArray = before.idArray;
        probe.recordCount = before.count;
        probe.publishedCount = positions.size();
        if (positions.empty()) {
            SetError(CoordinateDecrypt3Error::NoPublishedCoordinates);
            return false;
        }
        SetError(CoordinateDecrypt3Error::None);
        return true;
    }

    bool Refresh(std::uintptr_t currentWorld) noexcept {
        if (!probe.active || memory == nullptr ||
            currentWorld == 0) {
            SetError(CoordinateDecrypt3Error::InvalidInput, -EINVAL);
            return false;
        }
        ++probe.pollCount;
        if (world != currentWorld) {
            world = currentWorld;
            ClearPublishedState();
            if (!ReArmPageTrap()) {
                SetError(CoordinateDecrypt3Error::ArmFailed);
                return false;
            }
        }

        std::uintptr_t managerAddress = 0;
        if (!AddOffset(world, kWorldManagerOffset, managerAddress) ||
            !memory->Read(
                managerAddress,
                &probe.manager,
                sizeof(probe.manager)) ||
            probe.manager == 0) {
            SetError(CoordinateDecrypt3Error::ManagerReadFailed);
            return false;
        }

        if (backend == CoordinateDecrypt3Backend::PageTrap) {
            static_cast<void>(PollPageTrap());
        } else if (backend ==
                   CoordinateDecrypt3Backend::PageExecution) {
            static_cast<void>(PollPageExecution());
        }
        return PublishCoordinates();
    }

    bool Lookup(std::uintptr_t mesh,
                std::uintptr_t expectedWorld,
                CoordinateDecrypt3Position& position) const noexcept {
        position = {};
        if (!probe.active || memory == nullptr ||
            mesh == 0 || expectedWorld == 0 ||
            expectedWorld != world) {
            return false;
        }
        std::uintptr_t idAddress = 0;
        std::uint32_t id = 0;
        if (!AddOffset(mesh, kMeshIdOffset, idAddress) ||
            !memory->Read(idAddress, &id, sizeof(id)) ||
            id == 0) {
            return false;
        }
        const auto found = positions.find(id);
        if (found == positions.end()) return false;
        position = found->second;
        return true;
    }

    bool Stop() noexcept {
        bool stopped = true;
        if (backend == CoordinateDecrypt3Backend::PageTrap &&
            carrierDescriptor >= 0) {
            std::uint32_t reset = 0;
            stopped = Submit(
                          coordinate_decrypt3_abi::kDisarmOperation,
                          &reset) == 0;
#if defined(__ANDROID__) || defined(__linux__)
            close(carrierDescriptor);
#endif
            carrierDescriptor = -1;
        } else if (
            backend == CoordinateDecrypt3Backend::PageExecution &&
            memory != nullptr) {
            stopped = memory->RemovePageExecutionBreakpoints();
        }

        backend = CoordinateDecrypt3Backend::None;
        probe = {};
        world = 0;
        ClearPublishedState();
        memory = nullptr;
        processId = -1;
        return stopped;
    }

    void Reset() noexcept {
        static_cast<void>(Stop());
    }
};

CoordinateDecrypt3Runtime::CoordinateDecrypt3Runtime()
    : impl_(std::make_unique<Impl>()) {}

CoordinateDecrypt3Runtime::~CoordinateDecrypt3Runtime() = default;

bool CoordinateDecrypt3Runtime::Start(
    MemoryTransport& memory,
    pid_t processId,
    std::uintptr_t instructionAddress) noexcept {
    try {
        return impl_ != nullptr &&
            impl_->Start(memory, processId, instructionAddress);
    } catch (...) {
        return false;
    }
}

bool CoordinateDecrypt3Runtime::Refresh(
    std::uintptr_t world) noexcept {
    try {
        return impl_ != nullptr && impl_->Refresh(world);
    } catch (...) {
        return false;
    }
}

bool CoordinateDecrypt3Runtime::Lookup(
    std::uintptr_t mesh,
    std::uintptr_t world,
    CoordinateDecrypt3Position& position) const noexcept {
    try {
        return impl_ != nullptr &&
            impl_->Lookup(mesh, world, position);
    } catch (...) {
        position = {};
        return false;
    }
}

bool CoordinateDecrypt3Runtime::Stop() noexcept {
    try {
        return impl_ == nullptr || impl_->Stop();
    } catch (...) {
        return false;
    }
}

void CoordinateDecrypt3Runtime::Reset() noexcept {
    if (impl_ == nullptr) return;
    try {
        impl_->Reset();
    } catch (...) {
    }
}

bool CoordinateDecrypt3Runtime::IsActive() const noexcept {
    return impl_ != nullptr && impl_->probe.active;
}

CoordinateDecrypt3Probe
CoordinateDecrypt3Runtime::Probe() const noexcept {
    return impl_ != nullptr ? impl_->probe : CoordinateDecrypt3Probe{};
}

}  // namespace lengjing::game::native

#endif
