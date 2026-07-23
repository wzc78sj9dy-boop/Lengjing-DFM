#include "test_support.h"

#include "game/native/HardwareBreakpointCoordinateRuntime.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using lengjing::game::native::ExecutionBreakpointRecord;
using lengjing::game::native::HardwareBreakpointCoordinate;
using lengjing::game::native::HardwareBreakpointCoordinateCallbacks;
using lengjing::game::native::HardwareBreakpointCoordinateRuntime;

constexpr std::size_t kRecordStride = 0x40;
constexpr std::size_t kXOffset = 0x30;
constexpr std::size_t kYOffset = 0x34;
constexpr std::size_t kZOffset = 0x38;

struct RecordBatch {
    std::vector<ExecutionBreakpointRecord> records;
    std::uintptr_t hitAddress = 0;
    std::size_t totalRecords = 0;
};

struct MemoryRead {
    std::uintptr_t address = 0;
    std::size_t size = 0;
};

struct FakeRuntimeTransport {
    std::vector<RecordBatch> batches;
    std::size_t nextBatch = 0;
    std::unordered_map<std::uintptr_t, std::vector<std::uint8_t>> memory;
    std::vector<MemoryRead> memoryReads;
    std::vector<std::uintptr_t> configuredAddresses;
    std::uintptr_t failingAddress = 0;
    std::size_t removeCount = 0;

    template <typename Value>
    void PutValue(std::uintptr_t address, const Value& value) {
        std::vector<std::uint8_t> bytes(sizeof(value));
        std::memcpy(bytes.data(), &value, sizeof(value));
        memory[address] = std::move(bytes);
    }

    void PutBytes(std::uintptr_t address,
                  const void* data,
                  std::size_t size) {
        std::vector<std::uint8_t> bytes(size);
        if (size != 0) std::memcpy(bytes.data(), data, size);
        memory[address] = std::move(bytes);
    }

    void AddBatch(std::vector<ExecutionBreakpointRecord> records,
                  std::uintptr_t hitAddress,
                  std::size_t totalRecords = 0) {
        if (totalRecords == 0) totalRecords = records.size();
        batches.push_back(
            RecordBatch{std::move(records), hitAddress, totalRecords});
    }

    bool SawRead(std::uintptr_t address, std::size_t size) const {
        for (const MemoryRead& read : memoryReads) {
            if (read.address == address && read.size == size) return true;
        }
        return false;
    }

    HardwareBreakpointCoordinateCallbacks Callbacks() {
        return {
            [this](std::uintptr_t address) {
                configuredAddresses.push_back(address);
                return true;
            },
            [this](ExecutionBreakpointRecord* records,
                   std::size_t capacity,
                   std::size_t& recordsRead,
                   std::uintptr_t& hitAddress,
                   std::size_t& totalRecords) {
                if (nextBatch >= batches.size()) return false;
                const RecordBatch& batch = batches[nextBatch++];
                if (batch.records.size() > capacity) return false;
                for (std::size_t index = 0;
                     index < batch.records.size();
                     ++index) {
                    records[index] = batch.records[index];
                }
                recordsRead = batch.records.size();
                hitAddress = batch.hitAddress;
                totalRecords = batch.totalRecords;
                return true;
            },
            [this](std::uintptr_t address,
                   void* destination,
                   std::size_t size) {
                memoryReads.push_back(MemoryRead{address, size});
                if (address == failingAddress) return false;
                for (const auto& entry : memory) {
                    if (address < entry.first ||
                        address - entry.first > entry.second.size() ||
                        size > entry.second.size() -
                            static_cast<std::size_t>(
                                address - entry.first)) {
                        continue;
                    }
                    std::memcpy(
                        destination,
                        entry.second.data() +
                            static_cast<std::size_t>(
                                address - entry.first),
                        size);
                    return true;
                }
                return false;
            },
            [this] {
                ++removeCount;
                return true;
            },
        };
    }
};

ExecutionBreakpointRecord Record(std::uint64_t hitCount,
                                 std::uintptr_t pc,
                                 std::uintptr_t x23,
                                 pid_t tid = 123) {
    ExecutionBreakpointRecord record{};
    record.tid = tid;
    record.hitCount = hitCount;
    record.pc = pc;
    record.sp = 0x11110000;
    record.x0 = 0x22220000;
    record.x23 = x23;
    return record;
}

void SetCoordinate(std::vector<std::uint8_t>& records,
                   std::size_t index,
                   float x,
                   float y,
                   float z) {
    const std::size_t offset = index * kRecordStride;
    std::memcpy(records.data() + offset + kXOffset, &x, sizeof(x));
    std::memcpy(records.data() + offset + kYOffset, &y, sizeof(y));
    std::memcpy(records.data() + offset + kZOffset, &z, sizeof(z));
}

void InstallTable(FakeRuntimeTransport& transport,
                  std::uintptr_t manager,
                  std::uintptr_t recordsBase,
                  std::uintptr_t idArray,
                  std::int32_t count,
                  const std::vector<std::uint8_t>* records = nullptr,
                  const std::vector<std::uint32_t>* ids = nullptr) {
    transport.PutValue(manager + 0xF98, idArray);
    transport.PutValue(manager + 0xFA0, count);
    if (count < 15 || count > 16384) return;

    const std::size_t itemCount = static_cast<std::size_t>(count);
    std::vector<std::uint8_t> emptyRecords(itemCount * kRecordStride);
    std::vector<std::uint32_t> emptyIds(itemCount);
    const std::vector<std::uint8_t>& selectedRecords =
        records != nullptr ? *records : emptyRecords;
    const std::vector<std::uint32_t>& selectedIds =
        ids != nullptr ? *ids : emptyIds;
    transport.PutBytes(
        recordsBase, selectedRecords.data(), selectedRecords.size());
    transport.PutBytes(
        idArray,
        selectedIds.data(),
        selectedIds.size() * sizeof(std::uint32_t));
}

void TestTablePublicationAndLifecycle() {
    constexpr std::uintptr_t kBreakpoint = 0x4000;
    constexpr std::uintptr_t kSecondBreakpoint = 0x4800;
    constexpr std::uintptr_t kWorld = 0x5000;
    constexpr std::uintptr_t kOtherWorld = 0x6000;
    constexpr std::uintptr_t kManager = 0x7000;
    constexpr std::uintptr_t kRecordsBase = 0x12345000;
    constexpr std::uintptr_t kTaggedRecordsBase =
        UINT64_C(0xAB00000012345000);
    constexpr std::uintptr_t kIdArray = 0x22345000;
    constexpr std::int32_t kCount = 15;

    FakeRuntimeTransport transport{};
    transport.PutValue(kWorld + 0x1B8, kManager);
    transport.PutValue(kOtherWorld + 0x1B8, kManager);

    std::vector<std::uint8_t> records(
        static_cast<std::size_t>(kCount) * kRecordStride);
    std::vector<std::uint32_t> ids(
        static_cast<std::size_t>(kCount));
    ids[0] = 101;
    SetCoordinate(records, 0, 1.0f, 2.0f, 3.0f);
    ids[1] = 102;
    SetCoordinate(records, 1, 0.0f, 2.0f, 3.0f);
    ids[2] = 103;
    SetCoordinate(records, 2, 1.0f, 0.0f, 3.0f);
    ids[3] = 104;
    SetCoordinate(records, 3, 1.0f, 2.0f, 0.0f);
    ids[4] = 105;
    SetCoordinate(
        records,
        4,
        std::numeric_limits<float>::quiet_NaN(),
        2.0f,
        3.0f);
    ids[5] = 106;
    SetCoordinate(
        records,
        5,
        1.0f,
        std::numeric_limits<float>::infinity(),
        3.0f);
    ids[6] = 0;
    SetCoordinate(records, 6, 4.0f, 5.0f, 6.0f);
    ids[7] = 107;
    SetCoordinate(records, 7, -1.0f, -2.0f, -3.0f);
    ids[8] = 101;
    SetCoordinate(records, 8, 10.0f, 20.0f, 30.0f);
    InstallTable(
        transport,
        kManager,
        kRecordsBase,
        kIdArray,
        kCount,
        &records,
        &ids);

    const ExecutionBreakpointRecord sample =
        Record(1, kBreakpoint, kTaggedRecordsBase);
    transport.AddBatch({sample}, kBreakpoint);

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(!runtime.Start(0, transport.Callbacks()));
    REQUIRE(!runtime.Start(kBreakpoint + 2, transport.Callbacks()));
    REQUIRE(runtime.Start(kBreakpoint, transport.Callbacks()));
    REQUIRE(transport.configuredAddresses.size() == 1);
    REQUIRE(transport.configuredAddresses[0] == kBreakpoint);
    REQUIRE(runtime.Poll(kWorld));
    REQUIRE(runtime.RecordsBase() == kRecordsBase);
    REQUIRE(runtime.AcceptedSampleCount() == 1);
    REQUIRE(runtime.PublishedCoordinateCount() == 5);
    REQUIRE(transport.SawRead(
        kRecordsBase,
        static_cast<std::size_t>(kCount) * kRecordStride));
    REQUIRE(transport.SawRead(
        kIdArray,
        static_cast<std::size_t>(kCount) * sizeof(std::uint32_t)));
    REQUIRE(transport.SawRead(kRecordsBase, sizeof(std::uint64_t)));
    REQUIRE(!transport.SawRead(sample.sp + 0x20, sizeof(std::uintptr_t)));
    REQUIRE(!transport.SawRead(
        sample.x0 + 0x10, sizeof(HardwareBreakpointCoordinate)));

    HardwareBreakpointCoordinate coordinate{};
    REQUIRE(runtime.Lookup(101, kWorld, coordinate));
    REQUIRE(coordinate.x == 10.0f);
    REQUIRE(coordinate.y == 20.0f);
    REQUIRE(coordinate.z == 110.0f);
    REQUIRE(runtime.Lookup(107, kWorld, coordinate));
    REQUIRE(coordinate.x == -1.0f);
    REQUIRE(coordinate.y == -2.0f);
    REQUIRE(coordinate.z == 77.0f);
    REQUIRE(!runtime.Lookup(0, kWorld, coordinate));
    REQUIRE(runtime.Lookup(102, kWorld, coordinate));
    REQUIRE(coordinate.x == 0.0f);
    REQUIRE(coordinate.y == 2.0f);
    REQUIRE(coordinate.z == 83.0f);
    REQUIRE(runtime.Lookup(103, kWorld, coordinate));
    REQUIRE(coordinate.x == 1.0f);
    REQUIRE(coordinate.y == 0.0f);
    REQUIRE(coordinate.z == 83.0f);
    REQUIRE(runtime.Lookup(104, kWorld, coordinate));
    REQUIRE(coordinate.x == 1.0f);
    REQUIRE(coordinate.y == 2.0f);
    REQUIRE(coordinate.z == 80.0f);
    REQUIRE(!runtime.Lookup(101, kOtherWorld, coordinate));

    transport.AddBatch({sample}, kBreakpoint);
    REQUIRE(runtime.Poll(kWorld));
    REQUIRE(runtime.AcceptedSampleCount() == 1);

    SetCoordinate(records, 8, 900.0f, 901.0f, 902.0f);
    transport.PutBytes(kRecordsBase, records.data(), records.size());
    transport.failingAddress = kIdArray;
    transport.AddBatch({sample}, kBreakpoint);
    REQUIRE(!runtime.Poll(kWorld));
    REQUIRE(runtime.Lookup(101, kWorld, coordinate));
    REQUIRE(coordinate.x == 10.0f);
    REQUIRE(coordinate.z == 110.0f);
    transport.failingAddress = 0;

    transport.AddBatch({sample}, kBreakpoint);
    REQUIRE(runtime.Poll(kOtherWorld, kManager));
    REQUIRE(runtime.AcceptedSampleCount() == 2);
    REQUIRE(runtime.Lookup(101, kOtherWorld, coordinate));
    REQUIRE(coordinate.x == 900.0f);
    REQUIRE(coordinate.z == 982.0f);
    REQUIRE(transport.SawRead(
        kOtherWorld + 0x1B8, sizeof(std::uintptr_t)));

    REQUIRE(runtime.Start(kSecondBreakpoint, transport.Callbacks()));
    REQUIRE(transport.configuredAddresses.back() == kSecondBreakpoint);
    REQUIRE(transport.removeCount == 1);
    REQUIRE(runtime.RecordsBase() == 0);
    REQUIRE(runtime.PublishedCoordinateCount() == 0);
    REQUIRE(runtime.AcceptedSampleCount() == 0);
    REQUIRE(!runtime.Lookup(101, kOtherWorld, coordinate));

    REQUIRE(runtime.Stop());
    REQUIRE(transport.removeCount == 2);
    REQUIRE(!runtime.IsActive());
}

void TestTenSlotModeAndFirstTie() {
    constexpr std::uintptr_t kBreakpoint = 0x8000;
    constexpr std::uintptr_t kWorld = 0x9000;
    constexpr std::uintptr_t kManager = 0xA000;
    constexpr std::uintptr_t kIdArray = 0xB000;
    constexpr std::uintptr_t kA = 0x101000;
    constexpr std::uintptr_t kB = 0x102000;
    constexpr std::uintptr_t kC = 0x103000;
    constexpr std::uintptr_t kD = 0x104000;
    constexpr std::uintptr_t kE = 0x105000;
    constexpr std::uintptr_t kF = 0x106000;

    FakeRuntimeTransport transport{};
    transport.PutValue(kWorld + 0x1B8, kManager);
    for (const std::uintptr_t base : {kA, kB, kC, kD, kE, kF}) {
        InstallTable(transport, kManager, base, kIdArray, 15);
    }

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(kBreakpoint, transport.Callbacks()));
    const std::vector<std::uintptr_t> sequence = {
        kA, kB, kA, kB, kC, kC, kD, kD, kE, kE,
    };
    for (std::size_t index = 0; index < sequence.size(); ++index) {
        const std::uintptr_t candidate = index == 0
            ? sequence[index] | UINT64_C(0xCD00000000000000)
            : sequence[index];
        transport.AddBatch(
            {Record(index + 1, kBreakpoint, candidate)},
            kBreakpoint);
        REQUIRE(runtime.Poll(kWorld, kManager));
    }
    REQUIRE(runtime.AcceptedSampleCount() == 10);
    REQUIRE(runtime.RecordsBase() == kA);

    transport.AddBatch(
        {Record(11, kBreakpoint, kF)},
        kBreakpoint);
    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.AcceptedSampleCount() == 11);
    REQUIRE(runtime.RecordsBase() == kB);

    transport.AddBatch(
        {Record(11, kBreakpoint, kF)},
        kBreakpoint);
    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.AcceptedSampleCount() == 11);
    REQUIRE(runtime.RecordsBase() == kB);

    runtime.ResetWorld(kWorld + 1);
    REQUIRE(runtime.RecordsBase() == 0);
    REQUIRE(runtime.PublishedCoordinateCount() == 0);
    REQUIRE(runtime.Stop());
}

bool PollWithCount(std::int32_t count) {
    constexpr std::uintptr_t kBreakpoint = 0xC000;
    constexpr std::uintptr_t kWorld = 0xD000;
    constexpr std::uintptr_t kManager = 0xE000;
    constexpr std::uintptr_t kRecordsBase = 0x200000;
    constexpr std::uintptr_t kIdArray = 0x400000;

    FakeRuntimeTransport transport{};
    transport.PutValue(kWorld + 0x1B8, kManager);
    InstallTable(
        transport,
        kManager,
        kRecordsBase,
        kIdArray,
        count);
    transport.AddBatch(
        {Record(1, kBreakpoint, kRecordsBase)},
        kBreakpoint);
    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(kBreakpoint, transport.Callbacks()));
    const bool result = runtime.Poll(kWorld, kManager);
    REQUIRE(runtime.Stop());
    return result;
}

void TestCountBoundariesAndZeroCandidate() {
    REQUIRE(!PollWithCount(14));
    REQUIRE(PollWithCount(15));
    REQUIRE(PollWithCount(16384));
    REQUIRE(!PollWithCount(16385));

    constexpr std::uintptr_t kBreakpoint = 0xF000;
    constexpr std::uintptr_t kWorld = 0x11000;
    constexpr std::uintptr_t kManager = 0x12000;
    FakeRuntimeTransport transport{};
    transport.AddBatch(
        {Record(
            1,
            kBreakpoint,
            UINT64_C(0xAB00000000000000))},
        kBreakpoint);
    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(kBreakpoint, transport.Callbacks()));
    REQUIRE(!runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.AcceptedSampleCount() == 0);
    REQUIRE(runtime.RecordsBase() == 0);
    REQUIRE(runtime.Stop());

    constexpr std::uintptr_t kReadableCandidate = 0x220000;
    constexpr std::uintptr_t kRejectedCandidate = 0xDEAD1000;
    FakeRuntimeTransport candidateTransport{};
    const std::uint64_t readableProbe = UINT64_C(0x1122334455667788);
    candidateTransport.PutValue(kReadableCandidate, readableProbe);
    candidateTransport.PutValue(kRejectedCandidate, readableProbe);
    candidateTransport.AddBatch(
        {
            Record(1, kBreakpoint, kRejectedCandidate),
            Record(2, kBreakpoint, kReadableCandidate + 0x1000),
        },
        kBreakpoint,
        2);
    HardwareBreakpointCoordinateRuntime candidateRuntime;
    REQUIRE(candidateRuntime.Start(
        kBreakpoint, candidateTransport.Callbacks()));
    REQUIRE(!candidateRuntime.Poll(kWorld, kManager));
    REQUIRE(candidateRuntime.AcceptedSampleCount() == 0);
    REQUIRE(candidateRuntime.RecordsBase() == 0);
    REQUIRE(candidateRuntime.Stop());
}

}  // namespace

void RunHardwareBreakpointCoordinateRuntimeTests() {
    TestTablePublicationAndLifecycle();
    TestTenSlotModeAndFirstTie();
    TestCountBoundariesAndZeroCandidate();
}
