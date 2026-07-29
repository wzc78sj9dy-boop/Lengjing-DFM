#include "test_support.h"

#include "game/native/HardwareBreakpointCoordinateRuntime.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using lengjing::game::native::ExecutionBreakpointRecord;
using lengjing::game::native::HardwareBreakpointCoordinate;
using lengjing::game::native::HardwareBreakpointCoordinateCallbacks;
using lengjing::game::native::HardwareBreakpointCoordinateProfile;
using lengjing::game::native::HardwareBreakpointCoordinateRuntime;

constexpr std::uintptr_t kManagerOffset = 0x1B8;
constexpr std::uintptr_t kTargetLinkOffset = 0x10;
constexpr std::uintptr_t kIdArrayOffset = 0xF98;
constexpr std::uintptr_t kCountOffset = 0xFA0;
constexpr std::uintptr_t kFirstLinkDelta = 0x100000;
constexpr std::uintptr_t kSecondLinkDelta = 0x200000;
constexpr std::size_t kRecordStride = 0x40;
constexpr std::size_t kCoordinateOffset = 0x30;

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
    std::function<void(std::uintptr_t, std::size_t)> beforeMemoryRead;
    std::uintptr_t failingAddress = 0;
    std::size_t removeCount = 0;
    bool removeResult = true;

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

    std::size_t ReadCount(std::uintptr_t address,
                          std::size_t size) const {
        std::size_t count = 0;
        for (const MemoryRead& read : memoryReads) {
            if (read.address == address && read.size == size) ++count;
        }
        return count;
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
                if (beforeMemoryRead) beforeMemoryRead(address, size);
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
                return removeResult;
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
    const HardwareBreakpointCoordinate coordinate{x, y, z};
    std::memcpy(
        records.data() + index * kRecordStride + kCoordinateOffset,
        &coordinate,
        sizeof(coordinate));
}

void InstallTable(FakeRuntimeTransport& transport,
                  std::uintptr_t manager,
                  std::uintptr_t recordsBase,
                  std::uintptr_t idArray,
                  std::int32_t count,
                  const std::vector<std::uint8_t>* records = nullptr,
                  const std::vector<std::uint32_t>* ids = nullptr) {
    transport.PutValue(manager + kIdArrayOffset, idArray);
    transport.PutValue(manager + kCountOffset, count);
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

void InstallOrderedTable(
    FakeRuntimeTransport& transport,
    std::uintptr_t manager,
    std::uintptr_t recordsBase,
    std::uintptr_t idArray,
    std::uint32_t rawCount,
    const std::vector<std::uint8_t>* records = nullptr,
    const std::vector<std::uint32_t>* ids = nullptr) {
    transport.PutValue(manager + kIdArrayOffset, idArray);
    transport.PutValue(manager + kCountOffset, rawCount);
    if (rawCount == 0) return;

    const std::size_t itemCount =
        static_cast<std::size_t>(rawCount - 1);
    if (itemCount < 15 || itemCount > 16384) return;

    std::vector<std::uint8_t> emptyRecords(
        itemCount * kRecordStride);
    std::vector<std::uint32_t> emptyIds(itemCount);
    const std::vector<std::uint8_t>& selectedRecords =
        records != nullptr ? *records : emptyRecords;
    const std::vector<std::uint32_t>& selectedIds =
        ids != nullptr ? *ids : emptyIds;
    REQUIRE(selectedRecords.size() == itemCount * kRecordStride);
    REQUIRE(selectedIds.size() == itemCount);
    transport.PutBytes(
        recordsBase, selectedRecords.data(), selectedRecords.size());
    transport.PutBytes(
        idArray,
        selectedIds.data(),
        selectedIds.size() * sizeof(std::uint32_t));
}

void InstallOrderedManagerChain(
    FakeRuntimeTransport& transport,
    std::uintptr_t targetBase,
    std::uintptr_t manager) {
    const std::uintptr_t first = targetBase + kFirstLinkDelta;
    const std::uintptr_t second = targetBase + kSecondLinkDelta;
    transport.PutValue(targetBase + kTargetLinkOffset, first);
    transport.PutValue(first + kTargetLinkOffset, second);
    transport.PutValue(second + kManagerOffset, manager);
}

void TestTablePublicationAndStability() {
    constexpr std::uintptr_t kBreakpoint = 0x4000;
    constexpr std::uintptr_t kSecondBreakpoint = 0x4800;
    constexpr std::uintptr_t kWorld = 0x5000;
    constexpr std::uintptr_t kOtherWorld = 0x6000;
    constexpr std::uintptr_t kManager = 0x7000;
    constexpr std::uintptr_t kRecordsBase = 0x12345000;
    constexpr std::uintptr_t kTaggedRecordsBase =
        UINT64_C(0xAB00000012345000);
    constexpr std::uintptr_t kIdArray = 0x22345000;
    constexpr std::uintptr_t kRacingIdArray = 0x22346000;
    constexpr std::int32_t kCount = 15;

    FakeRuntimeTransport transport{};
    transport.PutValue(kWorld + kManagerOffset, kManager);
    transport.PutValue(kOtherWorld + kManagerOffset, kManager);

    std::vector<std::uint8_t> records(
        static_cast<std::size_t>(kCount) * kRecordStride);
    std::vector<std::uint32_t> ids(static_cast<std::size_t>(kCount));
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
    ids[6] = 107;
    SetCoordinate(records, 6, 0.0f, 0.0f, 0.0f);
    ids[7] = 0;
    SetCoordinate(records, 7, 4.0f, 5.0f, 6.0f);
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
    REQUIRE(runtime.Poll(kWorld));
    REQUIRE(runtime.RecordsBase() == kRecordsBase);
    REQUIRE(runtime.AcceptedSampleCount() == 1);
    REQUIRE(runtime.PublishedCoordinateCount() == 4);
    REQUIRE(transport.ReadCount(kRecordsBase, sizeof(std::uint64_t)) == 1);
    REQUIRE(transport.ReadCount(
                kWorld + kManagerOffset,
                sizeof(std::uintptr_t)) == 2);
    REQUIRE(transport.ReadCount(
                kRecordsBase,
                static_cast<std::size_t>(kCount) * kRecordStride) == 1);
    REQUIRE(transport.ReadCount(
                kIdArray,
                static_cast<std::size_t>(kCount) *
                    sizeof(std::uint32_t)) == 1);

    HardwareBreakpointCoordinate coordinate{};
    REQUIRE(runtime.Lookup(101, kWorld, coordinate));
    REQUIRE(coordinate.x == 10.0f);
    REQUIRE(coordinate.y == 20.0f);
    REQUIRE(coordinate.z == 110.0f);
    REQUIRE(runtime.Lookup(101, 0xDEADBEEF, kWorld, coordinate));
    REQUIRE(runtime.Lookup(102, kWorld, coordinate));
    REQUIRE(coordinate.x == 0.0f);
    REQUIRE(coordinate.y == 2.0f);
    REQUIRE(coordinate.z == 83.0f);
    REQUIRE(runtime.Lookup(103, kWorld, coordinate));
    REQUIRE(coordinate.x == 1.0f);
    REQUIRE(coordinate.y == 0.0f);
    REQUIRE(coordinate.z == 83.0f);
    REQUIRE(runtime.Lookup(104, kWorld, coordinate));
    REQUIRE(coordinate.z == 80.0f);
    REQUIRE(!runtime.Lookup(105, kWorld, coordinate));
    REQUIRE(!runtime.Lookup(106, kWorld, coordinate));
    REQUIRE(!runtime.Lookup(107, kWorld, coordinate));

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

    bool raced = false;
    transport.beforeMemoryRead =
        [&](std::uintptr_t address, std::size_t size) {
            if (!raced && address == kIdArray &&
                size == static_cast<std::size_t>(kCount) *
                    sizeof(std::uint32_t)) {
                raced = true;
                transport.PutValue(
                    kManager + kIdArrayOffset, kRacingIdArray);
            }
        };
    transport.AddBatch({sample}, kBreakpoint);
    REQUIRE(!runtime.Poll(kWorld));
    REQUIRE(raced);
    REQUIRE(runtime.Lookup(101, kWorld, coordinate));
    REQUIRE(coordinate.x == 10.0f);
    transport.beforeMemoryRead = {};
    transport.PutValue(kManager + kIdArrayOffset, kIdArray);

    transport.AddBatch({sample}, kBreakpoint);
    REQUIRE(!runtime.Poll(kWorld, kManager + 0x1000));
    REQUIRE(runtime.Lookup(101, kWorld, coordinate));
    REQUIRE(coordinate.x == 10.0f);

    transport.AddBatch({sample}, kBreakpoint);
    REQUIRE(runtime.Poll(kOtherWorld, kManager));
    REQUIRE(runtime.AcceptedSampleCount() == 2);
    REQUIRE(runtime.Lookup(101, kOtherWorld, coordinate));
    REQUIRE(coordinate.x == 900.0f);
    REQUIRE(coordinate.z == 982.0f);

    REQUIRE(runtime.Start(kSecondBreakpoint, transport.Callbacks()));
    REQUIRE(transport.configuredAddresses.back() == kSecondBreakpoint);
    REQUIRE(transport.removeCount == 1);
    REQUIRE(runtime.RecordsBase() == 0);
    REQUIRE(runtime.PublishedCoordinateCount() == 0);
    REQUIRE(runtime.AcceptedSampleCount() == 0);
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
    transport.PutValue(kWorld + kManagerOffset, kManager);
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
    REQUIRE(runtime.Stop());
}

bool PollWithCount(std::int32_t count) {
    constexpr std::uintptr_t kBreakpoint = 0xC000;
    constexpr std::uintptr_t kWorld = 0xD000;
    constexpr std::uintptr_t kManager = 0xE000;
    constexpr std::uintptr_t kRecordsBase = 0x200000;
    constexpr std::uintptr_t kIdArray = 0x400000;

    FakeRuntimeTransport transport{};
    transport.PutValue(kWorld + kManagerOffset, kManager);
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

void TestCountAndCandidateBoundaries() {
    REQUIRE(!PollWithCount(14));
    REQUIRE(PollWithCount(15));
    REQUIRE(PollWithCount(16384));
    REQUIRE(!PollWithCount(16385));

    constexpr std::uintptr_t kBreakpoint = 0xF000;
    constexpr std::uintptr_t kWorld = 0x11000;
    constexpr std::uintptr_t kManager = 0x12000;
    constexpr std::uintptr_t kRejectedCandidate = 0xDEAD1000;
    constexpr std::uintptr_t kUnreadableCandidate = 0x220000;
    FakeRuntimeTransport transport{};
    const std::uint64_t probe = UINT64_C(0x1122334455667788);
    transport.PutValue(kRejectedCandidate, probe);
    transport.AddBatch(
        {
            Record(1, kBreakpoint, UINT64_C(0xAB00000000000000)),
            Record(2, kBreakpoint, kRejectedCandidate),
            Record(3, kBreakpoint, kUnreadableCandidate),
        },
        kBreakpoint,
        3);

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(kBreakpoint, transport.Callbacks()));
    REQUIRE(!runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.AcceptedSampleCount() == 0);
    REQUIRE(runtime.RecordsBase() == 0);
    REQUIRE(transport.ReadCount(
                kRejectedCandidate, sizeof(std::uint64_t)) == 0);
    REQUIRE(transport.ReadCount(
                kUnreadableCandidate, sizeof(std::uint64_t)) == 1);
    REQUIRE(runtime.Stop());
}

void RequireOrderedRawCount(std::uint32_t rawCount,
                            bool expectedResult) {
    constexpr std::uintptr_t kBreakpoint = 0x14000;
    constexpr std::uintptr_t kWorld = 0x15000;
    constexpr std::uintptr_t kManager =
        UINT64_C(0x6000010000);
    constexpr std::uintptr_t kRecordsBase =
        UINT64_C(0x6000100000);
    constexpr std::uintptr_t kIdArray =
        UINT64_C(0x6000300000);

    FakeRuntimeTransport transport{};
    InstallOrderedManagerChain(transport, kWorld, kManager);
    const std::uint64_t probe = UINT64_C(0x1122334455667788);
    transport.PutValue(kRecordsBase, probe);
    InstallOrderedTable(
        transport,
        kManager,
        kRecordsBase,
        kIdArray,
        rawCount);
    transport.AddBatch(
        {Record(1, kBreakpoint, kRecordsBase)},
        kBreakpoint);

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(runtime.Poll(kWorld, kManager) == expectedResult);
    if (expectedResult) {
        const std::size_t count =
            static_cast<std::size_t>(rawCount - 1);
        REQUIRE(transport.ReadCount(
                    kRecordsBase,
                    (count - 1) * kRecordStride) == 1);
        REQUIRE(transport.ReadCount(
                    kIdArray,
                    count * sizeof(std::uint32_t)) == 1);
    }
    REQUIRE(runtime.Stop());
}

void TestOrderedRawCountBoundaries() {
    RequireOrderedRawCount(15, false);
    RequireOrderedRawCount(16, true);
    RequireOrderedRawCount(16385, true);
    RequireOrderedRawCount(16386, false);
}

void TestOrderedRecordReadAndCoordinateValidation() {
    constexpr std::uintptr_t kBreakpoint = 0x16000;
    constexpr std::uintptr_t kWorld = 0x17000;
    constexpr std::uintptr_t kManager =
        UINT64_C(0x6001010000);
    constexpr std::uintptr_t kRecordsBase =
        UINT64_C(0x6001100000);
    constexpr std::uintptr_t kIdArray =
        UINT64_C(0x6001300000);
    constexpr std::uint32_t kRawCount = 17;
    constexpr std::size_t kCount =
        static_cast<std::size_t>(kRawCount - 1);

    std::vector<std::uint8_t> records(kCount * kRecordStride);
    std::vector<std::uint32_t> ids(kCount);
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
        1.0f,
        2.0f,
        std::numeric_limits<float>::infinity());
    ids[5] = 106;
    SetCoordinate(
        records,
        5,
        1.0f,
        2.0f,
        std::numeric_limits<float>::quiet_NaN());
    ids[kCount - 1] = 199;
    SetCoordinate(records, kCount - 1, 7.0f, 8.0f, 9.0f);

    FakeRuntimeTransport transport{};
    InstallOrderedManagerChain(transport, kWorld, kManager);
    InstallOrderedTable(
        transport,
        kManager,
        kRecordsBase,
        kIdArray,
        kRawCount,
        &records,
        &ids);
    transport.AddBatch(
        {Record(1, kBreakpoint, kRecordsBase)},
        kBreakpoint);

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(transport.ReadCount(
                kRecordsBase,
                (kCount - 1) * kRecordStride) == 1);
    REQUIRE(transport.ReadCount(
                kRecordsBase,
                kCount * kRecordStride) == 0);
    REQUIRE(transport.ReadCount(
                kWorld + kTargetLinkOffset,
                sizeof(std::uintptr_t)) == 2);
    REQUIRE(transport.ReadCount(
                kWorld + kFirstLinkDelta + kTargetLinkOffset,
                sizeof(std::uintptr_t)) == 2);
    REQUIRE(transport.ReadCount(
                kWorld + kSecondLinkDelta + kManagerOffset,
                sizeof(std::uintptr_t)) == 2);
    REQUIRE(transport.ReadCount(
                kWorld + kManagerOffset,
                sizeof(std::uintptr_t)) == 0);
    REQUIRE(runtime.PublishedCoordinateCount() == 1);

    HardwareBreakpointCoordinate coordinate{};
    REQUIRE(runtime.Lookup(101, kWorld, coordinate));
    REQUIRE(coordinate.x == 1.0f);
    REQUIRE(coordinate.y == 2.0f);
    REQUIRE(coordinate.z == 83.0f);
    REQUIRE(!runtime.Lookup(102, kWorld, coordinate));
    REQUIRE(!runtime.Lookup(103, kWorld, coordinate));
    REQUIRE(!runtime.Lookup(104, kWorld, coordinate));
    REQUIRE(!runtime.Lookup(105, kWorld, coordinate));
    REQUIRE(!runtime.Lookup(106, kWorld, coordinate));
    REQUIRE(!runtime.Lookup(199, kWorld, coordinate));

    std::fill(records.begin(), records.end(), std::uint8_t{0});
    InstallOrderedTable(
        transport,
        kManager,
        kRecordsBase,
        kIdArray,
        kRawCount,
        &records,
        &ids);
    transport.AddBatch(
        {Record(2, kBreakpoint, kRecordsBase)},
        kBreakpoint);
    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.PublishedCoordinateCount() == 1);
    REQUIRE(runtime.Lookup(101, kWorld, coordinate));
    REQUIRE(coordinate.x == 1.0f);
    REQUIRE(coordinate.y == 2.0f);
    REQUIRE(coordinate.z == 83.0f);
    REQUIRE(runtime.Stop());
}

void RequireOrderedCandidateAndRecordsBound(
    std::uintptr_t recordsBase,
    bool expectedResult) {
    constexpr std::uintptr_t kBreakpoint = 0x18000;
    constexpr std::uintptr_t kWorld = 0x19000;
    constexpr std::uintptr_t kManager =
        UINT64_C(0x6002010000);
    constexpr std::uintptr_t kIdArray =
        UINT64_C(0x6002300000);

    FakeRuntimeTransport transport{};
    InstallOrderedManagerChain(transport, kWorld, kManager);
    InstallOrderedTable(
        transport,
        kManager,
        recordsBase,
        kIdArray,
        16);
    transport.AddBatch(
        {Record(1, kBreakpoint, recordsBase)},
        kBreakpoint);

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(runtime.Poll(kWorld, kManager) == expectedResult);
    REQUIRE(runtime.AcceptedSampleCount() ==
            (expectedResult ? 1U : 0U));
    REQUIRE(runtime.RecordsBase() ==
            (expectedResult ? recordsBase : 0U));
    REQUIRE(transport.ReadCount(
                recordsBase, sizeof(std::uint64_t)) ==
            (expectedResult ? 1U : 0U));
    REQUIRE(runtime.Stop());
}

void RequireOrderedManagerBound(std::uintptr_t manager,
                                bool expectedResult) {
    constexpr std::uintptr_t kBreakpoint = 0x1A000;
    constexpr std::uintptr_t kWorld = 0x1B000;
    constexpr std::uintptr_t kRecordsBase =
        UINT64_C(0x6003100000);
    constexpr std::uintptr_t kIdArray =
        UINT64_C(0x6003300000);

    FakeRuntimeTransport transport{};
    InstallOrderedManagerChain(transport, kWorld, manager);
    InstallOrderedTable(
        transport,
        manager,
        kRecordsBase,
        kIdArray,
        16);
    transport.AddBatch(
        {Record(1, kBreakpoint, kRecordsBase)},
        kBreakpoint);

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(runtime.Poll(kWorld) == expectedResult);
    REQUIRE(runtime.AcceptedSampleCount() == 1);
    REQUIRE(runtime.RecordsBase() == kRecordsBase);
    REQUIRE(transport.ReadCount(
                manager + kIdArrayOffset,
                sizeof(std::uintptr_t)) ==
            (expectedResult ? 2U : 0U));
    REQUIRE(runtime.Stop());
}

void RequireOrderedIdArrayBound(std::uintptr_t idArray,
                                bool expectedResult) {
    constexpr std::uintptr_t kBreakpoint = 0x1C000;
    constexpr std::uintptr_t kWorld = 0x1D000;
    constexpr std::uintptr_t kManager =
        UINT64_C(0x6004010000);
    constexpr std::uintptr_t kRecordsBase =
        UINT64_C(0x6004100000);
    constexpr std::size_t kCount = 15;

    FakeRuntimeTransport transport{};
    InstallOrderedManagerChain(transport, kWorld, kManager);
    InstallOrderedTable(
        transport,
        kManager,
        kRecordsBase,
        idArray,
        16);
    transport.AddBatch(
        {Record(1, kBreakpoint, kRecordsBase)},
        kBreakpoint);

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(runtime.Poll(kWorld, kManager) == expectedResult);
    REQUIRE(transport.ReadCount(
                idArray,
                kCount * sizeof(std::uint32_t)) ==
            (expectedResult ? 1U : 0U));
    REQUIRE(runtime.Stop());
}

void TestOrderedStrictPointerBounds() {
    constexpr std::uintptr_t kLower = UINT64_C(0x5FEEE000FF);
    constexpr std::uintptr_t kUpper = UINT64_C(0x8000000330);

    RequireOrderedCandidateAndRecordsBound(kLower, false);
    RequireOrderedCandidateAndRecordsBound(kLower + 1, true);
    RequireOrderedCandidateAndRecordsBound(kUpper - 1, true);
    RequireOrderedCandidateAndRecordsBound(kUpper, false);

    RequireOrderedManagerBound(kLower, false);
    RequireOrderedManagerBound(kLower + 1, true);
    RequireOrderedManagerBound(kUpper - 1, true);
    RequireOrderedManagerBound(kUpper, false);

    RequireOrderedIdArrayBound(kLower, false);
    RequireOrderedIdArrayBound(kLower + 1, true);
    RequireOrderedIdArrayBound(kUpper - 1, true);
    RequireOrderedIdArrayBound(kUpper, false);
}

void TestOrderedReadFailureAndReconfigurePreserveState() {
    constexpr std::uintptr_t kBreakpoint = 0x1E000;
    constexpr std::uintptr_t kSecondBreakpoint = 0x1F000;
    constexpr std::uintptr_t kWorld = 0x20000;
    constexpr std::uintptr_t kManager =
        UINT64_C(0x6005010000);
    constexpr std::uintptr_t kRecordsA =
        UINT64_C(0x6005100000);
    constexpr std::uintptr_t kRecordsB =
        UINT64_C(0x6005200000);
    constexpr std::uintptr_t kIdArray =
        UINT64_C(0x6005300000);
    constexpr std::uint32_t kRawCount = 16;
    constexpr std::size_t kCount =
        static_cast<std::size_t>(kRawCount - 1);
    constexpr std::uint32_t kId = 701;

    std::vector<std::uint8_t> recordsA(kCount * kRecordStride);
    std::vector<std::uint8_t> recordsB(kCount * kRecordStride);
    std::vector<std::uint32_t> ids(kCount);
    ids[0] = kId;
    SetCoordinate(recordsA, 0, 1.0f, 2.0f, 3.0f);
    SetCoordinate(recordsB, 0, 10.0f, 20.0f, 30.0f);

    FakeRuntimeTransport transport{};
    InstallOrderedManagerChain(transport, kWorld, kManager);
    InstallOrderedTable(
        transport,
        kManager,
        kRecordsA,
        kIdArray,
        kRawCount,
        &recordsA,
        &ids);
    InstallOrderedTable(
        transport,
        kManager,
        kRecordsB,
        kIdArray,
        kRawCount,
        &recordsB,
        &ids);
    transport.AddBatch(
        {Record(1, kBreakpoint, kRecordsA)},
        kBreakpoint);
    transport.AddBatch(
        {Record(2, kBreakpoint, kRecordsA)},
        kBreakpoint);
    transport.AddBatch(
        {Record(3, kBreakpoint, kRecordsB)},
        kBreakpoint);

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.RecordsBase() == kRecordsA);
    REQUIRE(runtime.AcceptedSampleCount() == 3);
    REQUIRE(runtime.PublishedCoordinateCount() == 1);

    HardwareBreakpointCoordinate coordinate{};
    REQUIRE(runtime.Lookup(kId, kWorld, coordinate));
    REQUIRE(coordinate.x == 1.0f);
    REQUIRE(coordinate.z == 83.0f);

    REQUIRE(!runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.NeedsReconfigure());
    REQUIRE(runtime.RecordsBase() == kRecordsA);
    REQUIRE(runtime.AcceptedSampleCount() == 3);
    REQUIRE(runtime.PublishedCoordinateCount() == 1);
    REQUIRE(runtime.Lookup(kId, kWorld, coordinate));
    REQUIRE(coordinate.x == 1.0f);

    REQUIRE(runtime.Reconfigure(
        kSecondBreakpoint, transport.Callbacks()));
    REQUIRE(!runtime.NeedsReconfigure());
    REQUIRE(runtime.BreakpointAddress() == kSecondBreakpoint);
    REQUIRE(transport.configuredAddresses.size() == 2);
    REQUIRE(transport.configuredAddresses.back() ==
            kSecondBreakpoint);
    REQUIRE(transport.removeCount == 1);
    REQUIRE(runtime.RecordsBase() == kRecordsA);
    REQUIRE(runtime.AcceptedSampleCount() == 3);
    REQUIRE(runtime.PublishedCoordinateCount() == 1);
    REQUIRE(runtime.Lookup(kId, kWorld, coordinate));
    REQUIRE(coordinate.x == 1.0f);

    transport.AddBatch(
        {Record(3, kSecondBreakpoint, kRecordsB)},
        kSecondBreakpoint);
    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.RecordsBase() == kRecordsA);
    REQUIRE(runtime.AcceptedSampleCount() == 3);

    transport.AddBatch(
        {Record(1, kSecondBreakpoint, kRecordsB)},
        kSecondBreakpoint);
    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.RecordsBase() == kRecordsA);
    REQUIRE(runtime.AcceptedSampleCount() == 4);
    REQUIRE(runtime.Lookup(kId, kWorld, coordinate));
    REQUIRE(coordinate.x == 1.0f);

    transport.AddBatch(
        {Record(2, kSecondBreakpoint, kRecordsB)},
        kSecondBreakpoint);
    transport.failingAddress = kIdArray;
    REQUIRE(!runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.RecordsBase() == kRecordsB);
    REQUIRE(runtime.AcceptedSampleCount() == 5);
    REQUIRE(runtime.Lookup(kId, kWorld, coordinate));
    REQUIRE(coordinate.x == 1.0f);
    REQUIRE(coordinate.z == 83.0f);

    transport.failingAddress = 0;
    transport.AddBatch(
        {Record(3, kSecondBreakpoint, kRecordsB)},
        kSecondBreakpoint);
    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.RecordsBase() == kRecordsB);
    REQUIRE(runtime.AcceptedSampleCount() == 6);
    REQUIRE(runtime.Lookup(kId, kWorld, coordinate));
    REQUIRE(coordinate.x == 10.0f);
    REQUIRE(coordinate.y == 20.0f);
    REQUIRE(coordinate.z == 110.0f);
    REQUIRE(runtime.Stop());
    REQUIRE(transport.removeCount == 2);
}

void TestOrderedRecordReorderAndCountRollback() {
    constexpr std::uintptr_t kBreakpoint = 0x21000;
    constexpr std::uintptr_t kWorld = 0x22000;
    constexpr std::uintptr_t kManager =
        UINT64_C(0x6006010000);
    constexpr std::uintptr_t kRecordsBase =
        UINT64_C(0x6006100000);
    constexpr std::uintptr_t kIdArray =
        UINT64_C(0x6006300000);

    FakeRuntimeTransport transport{};
    InstallOrderedManagerChain(transport, kWorld, kManager);
    InstallOrderedTable(
        transport,
        kManager,
        kRecordsBase,
        kIdArray,
        16);
    const ExecutionBreakpointRecord first =
        Record(1, kBreakpoint, kRecordsBase, 101);
    const ExecutionBreakpointRecord second =
        Record(1, kBreakpoint, kRecordsBase, 202);
    transport.AddBatch({first, second}, kBreakpoint, 2);
    transport.AddBatch({second, first}, kBreakpoint, 2);
    transport.AddBatch(
        {Record(2, kBreakpoint, kRecordsBase, 101)},
        kBreakpoint,
        1);

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.AcceptedSampleCount() == 2);
    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.AcceptedSampleCount() == 2);
    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.AcceptedSampleCount() == 3);
    REQUIRE(!runtime.NeedsReconfigure());
    REQUIRE(runtime.RecordsBase() == kRecordsBase);
    REQUIRE(transport.configuredAddresses.size() == 1);
    REQUIRE(runtime.Stop());
}

void TestOrderedReconfigureRequiresRemoval() {
    constexpr std::uintptr_t kBreakpoint = 0x23000;
    constexpr std::uintptr_t kSecondBreakpoint = 0x24000;

    FakeRuntimeTransport transport{};
    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));

    transport.removeResult = false;
    REQUIRE(!runtime.Reconfigure(
        kSecondBreakpoint, transport.Callbacks()));
    REQUIRE(runtime.NeedsReconfigure());
    REQUIRE(runtime.BreakpointAddress() == kBreakpoint);
    REQUIRE(transport.configuredAddresses.size() == 1);
    REQUIRE(transport.removeCount == 1);

    transport.removeResult = true;
    REQUIRE(runtime.Reconfigure(
        kSecondBreakpoint, transport.Callbacks()));
    REQUIRE(!runtime.NeedsReconfigure());
    REQUIRE(runtime.BreakpointAddress() == kSecondBreakpoint);
    REQUIRE(transport.configuredAddresses.size() == 2);
    REQUIRE(transport.removeCount == 2);
    REQUIRE(runtime.Stop());
    REQUIRE(transport.removeCount == 3);
}

}  // namespace

void RunHardwareBreakpointCoordinateRuntimeTests() {
    TestTablePublicationAndStability();
    TestTenSlotModeAndFirstTie();
    TestCountAndCandidateBoundaries();
    TestOrderedRawCountBoundaries();
    TestOrderedRecordReadAndCoordinateValidation();
    TestOrderedStrictPointerBounds();
    TestOrderedReadFailureAndReconfigurePreserveState();
    TestOrderedRecordReorderAndCountRollback();
    TestOrderedReconfigureRequiresRemoval();
}
