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
using lengjing::game::native::HardwareBreakpointCandidateSampleResult;
using lengjing::game::native::HardwareBreakpointCoordinateCallbacks;
using lengjing::game::native::HardwareBreakpointCoordinateProfile;
using lengjing::game::native::HardwareBreakpointCoordinateRuntime;

constexpr std::uintptr_t kManagerOffset = 0x1B8;
constexpr std::uintptr_t kTargetLinkOffset = 0x10;
constexpr std::uintptr_t kIdArrayOffset = 0xF98;
constexpr std::uintptr_t kCountOffset = 0xFA0;
constexpr std::uintptr_t kMeshCoordinateOffset = 0x80;
constexpr std::uintptr_t kMeshIdOffset = 0x2D8;
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
            {},
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
    record.x20 =
        (x23 & UINT64_C(0x00FFFFFFFFFFFFFF)) + UINT64_C(0x280);
    record.x23 = x23;
    return record;
}

ExecutionBreakpointRecord MeshRecord(std::uint64_t hitCount,
                                     std::uintptr_t pc,
                                     std::uintptr_t coordinateBase,
                                     std::uintptr_t mesh,
                                     std::uintptr_t x23,
                                     pid_t tid = 123) {
    ExecutionBreakpointRecord record =
        Record(hitCount, pc, x23, tid);
    record.x20 = coordinateBase;
    record.x21 = mesh;
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
    const std::size_t itemCount =
        static_cast<std::size_t>(rawCount);
    if (itemCount < 15 || itemCount > 16384) return;

    std::vector<std::uint8_t> emptyRecords(
        itemCount * kRecordStride);
    std::vector<std::uint32_t> emptyIds(itemCount);
    if (records == nullptr && ids == nullptr) {
        const std::size_t populated = std::min(
            itemCount, static_cast<std::size_t>(32));
        for (std::size_t index = 0; index < populated; ++index) {
            emptyIds[index] =
                static_cast<std::uint32_t>(1000U + index);
            SetCoordinate(
                emptyRecords,
                index,
                100.0f + static_cast<float>(index) * 10.0f,
                200.0f + static_cast<float>(index) * 10.0f,
                300.0f + static_cast<float>(index) * 10.0f);
        }
    }
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

std::vector<std::uint32_t> MakeSequentialIds(
    std::size_t itemCount,
    std::uint32_t firstId) {
    std::vector<std::uint32_t> ids(itemCount);
    for (std::size_t index = 0; index < itemCount; ++index) {
        ids[index] =
            firstId + static_cast<std::uint32_t>(index);
    }
    return ids;
}

std::vector<std::uint8_t> MakeCoordinateRecords(
    std::size_t itemCount,
    std::size_t firstIndex,
    std::size_t populatedCount,
    float base) {
    std::vector<std::uint8_t> records(itemCount * kRecordStride);
    for (std::size_t offset = 0; offset < populatedCount; ++offset) {
        const std::size_t index = firstIndex + offset;
        const float delta = static_cast<float>(offset) * 10.0f;
        SetCoordinate(
            records,
            index,
            base + delta,
            base + 100.0f + delta,
            base + 200.0f + delta);
    }
    return records;
}

void TestStableRecordTableUsesNewestRecordPerThread() {
    constexpr std::uintptr_t kBreakpoint = 0x3C000;
    constexpr std::uintptr_t kWorld = 0x3D000;
    constexpr std::uintptr_t kTargetRoot =
        UINT64_C(0x6030010000);
    constexpr std::uintptr_t kIgnoredManager =
        UINT64_C(0x6030020000);
    constexpr std::uintptr_t kManager =
        UINT64_C(0x6030100000);
    constexpr std::uintptr_t kRecordsBase =
        UINT64_C(0x6030200000);
    constexpr std::uintptr_t kOldNoiseBase =
        UINT64_C(0x6030300000);
    constexpr std::uintptr_t kIdArray =
        UINT64_C(0x6030500000);
    constexpr std::uintptr_t kTaggedRecordsBase =
        UINT64_C(0xAB00000000000000) | kRecordsBase;
    constexpr std::int32_t kCount = 16;
    constexpr std::uint32_t kId = 8201;

    std::vector<std::uint8_t> coordinateRecords(
        static_cast<std::size_t>(kCount) * kRecordStride);
    std::vector<std::uint32_t> ids(
        static_cast<std::size_t>(kCount));
    ids[0] = kId;
    SetCoordinate(coordinateRecords, 0, 100.0f, 200.0f, 30.0f);

    FakeRuntimeTransport transport{};
    transport.PutValue(kWorld + kManagerOffset, kManager);
    transport.PutValue(
        kTargetRoot + kTargetLinkOffset, std::uintptr_t{0});
    transport.PutValue(
        kTargetRoot + kManagerOffset, kIgnoredManager);
    InstallTable(
        transport,
        kManager,
        kRecordsBase,
        kIdArray,
        kCount,
        &coordinateRecords,
        &ids);
    transport.PutValue(
        kOldNoiseBase, UINT64_C(0x1111222233334444));

    std::vector<ExecutionBreakpointRecord> history;
    for (std::uint64_t hit = 1; hit <= 254; ++hit) {
        history.push_back(
            Record(hit, kBreakpoint, kOldNoiseBase, 500));
    }
    history.push_back(
        Record(255, kBreakpoint, kTaggedRecordsBase, 500));
    history.push_back(
        Record(1, kBreakpoint, kTaggedRecordsBase, 501));
    REQUIRE(history.size() ==
            lengjing::game::native::kExecutionBreakpointRecordLimit);
    transport.AddBatch(history, kBreakpoint);
    transport.AddBatch(history, kBreakpoint);

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::StableRecordTable));
    REQUIRE(runtime.Poll(kWorld, kTargetRoot, kIgnoredManager));
    REQUIRE(runtime.AcceptedSampleCount() == 2);
    REQUIRE(runtime.RecordsBase() == kRecordsBase);
    REQUIRE(runtime.PublishedCoordinateCount() == 1);

    HardwareBreakpointCoordinate coordinate{};
    REQUIRE(runtime.Lookup(kId, kWorld, coordinate));
    REQUIRE(coordinate.x == 100.0f);
    REQUIRE(coordinate.y == 200.0f);
    REQUIRE(coordinate.z == 110.0f);
    REQUIRE(transport.ReadCount(
                kOldNoiseBase, sizeof(std::uint64_t)) == 0);
    REQUIRE(transport.ReadCount(
                kRecordsBase, sizeof(std::uint64_t)) == 2);
    REQUIRE(transport.ReadCount(
                kWorld + kManagerOffset,
                sizeof(std::uintptr_t)) == 2);
    REQUIRE(transport.ReadCount(
                kTargetRoot + kManagerOffset,
                sizeof(std::uintptr_t)) == 0);
    REQUIRE(transport.ReadCount(
                kTargetRoot + kTargetLinkOffset,
                sizeof(std::uintptr_t)) == 0);

    REQUIRE(runtime.Poll(kWorld, kTargetRoot, kIgnoredManager));
    REQUIRE(runtime.AcceptedSampleCount() == 2);
    REQUIRE(runtime.RecordsBase() == kRecordsBase);
    REQUIRE(runtime.Lookup(kId, kWorld, coordinate));
    REQUIRE(transport.ReadCount(
                kWorld + kManagerOffset,
                sizeof(std::uintptr_t)) == 4);
    REQUIRE(runtime.Stop());
}

void TestMeshStreamUsesRegisterPairAndRejectsX23Table() {
    constexpr std::uintptr_t kBreakpoint = 0x3A000;
    constexpr std::uintptr_t kWorld = 0x3B000;
    constexpr std::uintptr_t kCoordinateBase =
        UINT64_C(0x6020100000);
    constexpr std::uintptr_t kMesh =
        UINT64_C(0x6020200000);
    constexpr std::uintptr_t kWrongRecordsBase =
        UINT64_C(0x6020300000);
    constexpr std::uintptr_t kManager =
        UINT64_C(0x6020400000);
    constexpr std::uint32_t kId = 8101;

    FakeRuntimeTransport transport{};
    transport.PutValue(kMesh + kMeshIdOffset, kId);
    const HardwareBreakpointCoordinate expected{120.0f, 0.0f, -30.0f};
    transport.PutValue(
        kCoordinateBase + kMeshCoordinateOffset, expected);
    const std::uint64_t wrongProbe = UINT64_C(0x1122334455667788);
    transport.PutValue(kWrongRecordsBase, wrongProbe);
    transport.PutValue(kManager + kIdArrayOffset, kWrongRecordsBase);
    transport.PutValue(kManager + kCountOffset, std::uint32_t{33});
    transport.AddBatch(
        {MeshRecord(
            1,
            kBreakpoint,
            kCoordinateBase,
            kMesh,
            kWrongRecordsBase)},
        kBreakpoint);

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::MeshStream));
    REQUIRE(runtime.Poll(kWorld, 0, kManager));
    REQUIRE(runtime.AcceptedSampleCount() == 1);
    REQUIRE(runtime.PublishedCoordinateCount() == 1);
    REQUIRE(runtime.RecordsBase() == kCoordinateBase);

    HardwareBreakpointCoordinate coordinate{};
    REQUIRE(!runtime.Lookup(kId, kWorld, coordinate));
    REQUIRE(runtime.Lookup(kId, kMesh, kWorld, coordinate));
    REQUIRE(coordinate.x == expected.x);
    REQUIRE(coordinate.y == expected.y);
    REQUIRE(coordinate.z == expected.z);
    REQUIRE(!runtime.Lookup(kId, kMesh + 0x1000, kWorld, coordinate));
    REQUIRE(transport.ReadCount(
                kMesh + kMeshIdOffset, sizeof(kId)) == 2);
    REQUIRE(transport.ReadCount(
                kCoordinateBase + kMeshCoordinateOffset,
                sizeof(expected)) == 1);
    REQUIRE(transport.ReadCount(
                kWrongRecordsBase, sizeof(wrongProbe)) == 0);
    REQUIRE(transport.ReadCount(
                kManager + kIdArrayOffset,
                sizeof(std::uintptr_t)) == 0);
    REQUIRE(transport.ReadCount(
                kManager + kCountOffset,
                sizeof(std::uint32_t)) == 0);

    constexpr std::uint32_t kRacingFirstId = 8102;
    constexpr std::uint32_t kRacingSecondId = 8103;
    transport.PutValue(kMesh + kMeshIdOffset, kRacingFirstId);
    std::size_t racingIdReads = 0;
    transport.beforeMemoryRead =
        [&](std::uintptr_t address, std::size_t size) {
            if (address != kMesh + kMeshIdOffset ||
                size != sizeof(std::uint32_t)) {
                return;
            }
            ++racingIdReads;
            if (racingIdReads == 2) {
                transport.PutValue(
                    kMesh + kMeshIdOffset, kRacingSecondId);
            }
        };
    transport.AddBatch(
        {MeshRecord(
            2,
            kBreakpoint,
            kCoordinateBase,
            kMesh,
            kWrongRecordsBase)},
        kBreakpoint);
    REQUIRE(runtime.Poll(kWorld, 0, kManager));
    REQUIRE(runtime.AcceptedSampleCount() == 1);
    REQUIRE(!runtime.Lookup(
        kRacingFirstId, kMesh, kWorld, coordinate));
    REQUIRE(!runtime.Lookup(
        kRacingSecondId, kMesh, kWorld, coordinate));
    REQUIRE(runtime.Lookup(kId, kMesh, kWorld, coordinate));

    runtime.ResetWorld(kWorld + 1);
    REQUIRE(runtime.PublishedCoordinateCount() == 0);
    REQUIRE(!runtime.Lookup(kId, kMesh, kWorld, coordinate));
    REQUIRE(runtime.Stop());
}

void TestMeshStreamMeshAndIdReuse() {
    constexpr std::uintptr_t kBreakpoint = 0x3C000;
    constexpr std::uintptr_t kWorld = 0x3D000;
    constexpr std::uintptr_t kCoordinateA =
        UINT64_C(0x6021100000);
    constexpr std::uintptr_t kCoordinateB =
        UINT64_C(0x6021200000);
    constexpr std::uintptr_t kCoordinateC =
        UINT64_C(0x6021250000);
    constexpr std::uintptr_t kMeshA =
        UINT64_C(0x6021300000);
    constexpr std::uintptr_t kMeshB =
        UINT64_C(0x6021400000);
    constexpr std::uintptr_t kMeshC =
        UINT64_C(0x6021500000);
    constexpr std::uint32_t kFirstId = 8201;
    constexpr std::uint32_t kSecondId = 8202;
    constexpr std::uint32_t kOtherId = 8203;

    FakeRuntimeTransport transport{};
    transport.PutValue(kMeshA + kMeshIdOffset, kFirstId);
    transport.PutValue(
        kCoordinateA + kMeshCoordinateOffset,
        HardwareBreakpointCoordinate{10.0f, 20.0f, 30.0f});
    transport.PutValue(kMeshC + kMeshIdOffset, kOtherId);
    transport.PutValue(
        kCoordinateC + kMeshCoordinateOffset,
        HardwareBreakpointCoordinate{-10.0f, -20.0f, -30.0f});
    transport.AddBatch(
        {
            MeshRecord(
                1, kBreakpoint, kCoordinateA, kMeshA, 0xDEAD1000, 123),
            MeshRecord(
                1, kBreakpoint, kCoordinateC, kMeshC, 0xDEAD1000, 124),
        },
        kBreakpoint);

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::MeshStream));
    REQUIRE(runtime.Poll(kWorld));
    HardwareBreakpointCoordinate coordinate{};
    REQUIRE(runtime.Lookup(
        kFirstId, kMeshA, kWorld, coordinate));
    REQUIRE(runtime.Lookup(
        kOtherId, kMeshC, kWorld, coordinate));
    REQUIRE(runtime.PublishedCoordinateCount() == 2);

    transport.PutValue(kMeshA + kMeshIdOffset, kSecondId);
    transport.PutValue(
        kCoordinateA + kMeshCoordinateOffset,
        HardwareBreakpointCoordinate{40.0f, 50.0f, 60.0f});
    transport.AddBatch(
        {MeshRecord(
            2, kBreakpoint, kCoordinateA, kMeshA, 0xDEAD2000)},
        kBreakpoint,
        2);
    REQUIRE(runtime.Poll(kWorld));
    REQUIRE(!runtime.Lookup(
        kFirstId, kMeshA, kWorld, coordinate));
    REQUIRE(runtime.Lookup(
        kSecondId, kMeshA, kWorld, coordinate));
    REQUIRE(coordinate.x == 40.0f);
    REQUIRE(runtime.Lookup(
        kOtherId, kMeshC, kWorld, coordinate));

    transport.PutValue(kMeshB + kMeshIdOffset, kSecondId);
    transport.PutValue(
        kCoordinateB + kMeshCoordinateOffset,
        HardwareBreakpointCoordinate{70.0f, 80.0f, 90.0f});
    transport.AddBatch(
        {MeshRecord(
            3, kBreakpoint, kCoordinateB, kMeshB, 0xDEAD3000)},
        kBreakpoint,
        2);
    REQUIRE(runtime.Poll(kWorld));
    REQUIRE(!runtime.Lookup(
        kSecondId, kMeshA, kWorld, coordinate));
    REQUIRE(runtime.Lookup(
        kSecondId, kMeshB, kWorld, coordinate));
    REQUIRE(coordinate.x == 70.0f);
    REQUIRE(runtime.Lookup(
        kOtherId, kMeshC, kWorld, coordinate));
    REQUIRE(runtime.PublishedCoordinateCount() == 2);
    REQUIRE(runtime.Stop());
}

void TestMeshStreamFreshnessAndProfileSwitch() {
    constexpr std::uintptr_t kBreakpoint = 0x3E000;
    constexpr std::uintptr_t kSecondBreakpoint = 0x3F000;
    constexpr std::uintptr_t kThirdBreakpoint = 0x40000;
    constexpr std::uintptr_t kWorld = 0x41000;
    constexpr std::uintptr_t kCoordinateBase =
        UINT64_C(0x6022100000);
    constexpr std::uintptr_t kMesh =
        UINT64_C(0x6022200000);
    constexpr std::uint32_t kId = 8301;

    FakeRuntimeTransport transport{};
    transport.PutValue(kMesh + kMeshIdOffset, kId);
    transport.PutValue(
        kCoordinateBase + kMeshCoordinateOffset,
        HardwareBreakpointCoordinate{1.0f, 2.0f, 3.0f});
    const ExecutionBreakpointRecord sample = MeshRecord(
        1, kBreakpoint, kCoordinateBase, kMesh, 0xDEAD4000);
    transport.AddBatch({sample}, kBreakpoint);
    for (std::size_t index = 0; index < 60; ++index) {
        transport.AddBatch({sample}, kBreakpoint);
    }

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::MeshStream));
    REQUIRE(runtime.Poll(kWorld));
    HardwareBreakpointCoordinate coordinate{};
    for (std::size_t index = 0; index < 59; ++index) {
        REQUIRE(runtime.Poll(kWorld));
    }
    REQUIRE(runtime.Lookup(kId, kMesh, kWorld, coordinate));
    REQUIRE(runtime.Poll(kWorld));
    REQUIRE(!runtime.Lookup(kId, kMesh, kWorld, coordinate));
    REQUIRE(runtime.PublishedCoordinateCount() == 0);

    transport.AddBatch(
        {MeshRecord(
            2,
            kBreakpoint,
            kCoordinateBase,
            kMesh,
            0xDEAD5000)},
        kBreakpoint);
    REQUIRE(runtime.Poll(kWorld));
    REQUIRE(runtime.PublishedCoordinateCount() == 1);
    REQUIRE(runtime.Reconfigure(
        kSecondBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(runtime.PublishedCoordinateCount() == 0);
    REQUIRE(runtime.PollCount() == 0);
    REQUIRE(runtime.AcceptedSampleCount() == 0);
    REQUIRE(!runtime.Poll(kWorld, 0, 0));

    transport.AddBatch(
        {MeshRecord(
            3,
            kThirdBreakpoint,
            kCoordinateBase,
            kMesh,
            0xDEAD6000)},
        kThirdBreakpoint);
    REQUIRE(runtime.Reconfigure(
        kThirdBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::MeshStream));
    REQUIRE(runtime.Poll(kWorld, 0, 0));
    REQUIRE(runtime.Lookup(kId, kMesh, kWorld, coordinate));
    REQUIRE(runtime.Stop());
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
    auto callbacks = transport.Callbacks();
    callbacks.readRecords = {};
    callbacks.readCandidate =
        [&](std::uintptr_t& candidate) {
            candidate = kRecordsBase;
            return HardwareBreakpointCandidateSampleResult::Accepted;
        };

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        callbacks,
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(runtime.Poll(kWorld, kManager) == expectedResult);
    if (expectedResult) {
        const std::size_t count =
            static_cast<std::size_t>(rawCount);
        REQUIRE(transport.ReadCount(
                    kRecordsBase,
                    count * kRecordStride) == 2);
        REQUIRE(transport.ReadCount(
                    kIdArray,
                    count * sizeof(std::uint32_t)) == 2);
    }
    REQUIRE(runtime.Stop());
}

void TestOrderedRawCountBoundaries() {
    RequireOrderedRawCount(15, true);
    RequireOrderedRawCount(16, true);
    RequireOrderedRawCount(16384, true);
    RequireOrderedRawCount(16385, false);
}

void TestOrderedExactSnapshotRejectsRecordRace() {
    constexpr std::uintptr_t kBreakpoint = 0x15400;
    constexpr std::uintptr_t kWorld = 0x15500;
    constexpr std::uintptr_t kManager =
        UINT64_C(0x6000410000);
    constexpr std::uintptr_t kRecordsBase =
        UINT64_C(0x6000420000);
    constexpr std::uintptr_t kIdArray =
        UINT64_C(0x6000430000);
    constexpr std::size_t kCount = 15;

    auto ids = MakeSequentialIds(kCount, 8500);
    auto records = MakeCoordinateRecords(kCount, 0, 1, 100.0f);
    FakeRuntimeTransport transport{};
    InstallOrderedManagerChain(transport, kWorld, kManager);
    InstallOrderedTable(
        transport,
        kManager,
        kRecordsBase,
        kIdArray,
        static_cast<std::uint32_t>(kCount),
        &records,
        &ids);
    auto callbacks = transport.Callbacks();
    callbacks.readRecords = {};
    callbacks.readCandidate =
        [&](std::uintptr_t& candidate) {
            candidate = kRecordsBase;
            return HardwareBreakpointCandidateSampleResult::Accepted;
        };

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        callbacks,
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(runtime.Poll(kWorld, kManager));
    HardwareBreakpointCoordinate coordinate{};
    REQUIRE(runtime.Lookup(8500, kWorld, coordinate));
    REQUIRE(coordinate.x == 100.0f);

    SetCoordinate(records, 0, 200.0f, 300.0f, 400.0f);
    transport.PutBytes(
        kRecordsBase, records.data(), records.size());
    auto raced = records;
    SetCoordinate(raced, 0, 2200.0f, 2300.0f, 2400.0f);
    std::size_t bulkReads = 0;
    transport.beforeMemoryRead =
        [&](std::uintptr_t address, std::size_t size) {
            if (address != kRecordsBase ||
                size != kCount * kRecordStride) {
                return;
            }
            ++bulkReads;
            if (bulkReads == 2) {
                transport.PutBytes(
                    kRecordsBase, raced.data(), raced.size());
            }
        };
    REQUIRE(!runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.Lookup(8500, kWorld, coordinate));
    REQUIRE(coordinate.x == 100.0f);
    REQUIRE(runtime.Stop());
}

void TestOrderedFallbackDoesNotRequireCursorShape() {
    constexpr std::uintptr_t kBreakpoint = 0x15800;
    constexpr std::uintptr_t kWorld = 0x15900;
    constexpr std::uintptr_t kManager =
        UINT64_C(0x6000510000);
    constexpr std::uintptr_t kRecordsBase =
        UINT64_C(0x6000600000);
    constexpr std::uintptr_t kIdArray =
        UINT64_C(0x6000800000);

    FakeRuntimeTransport transport{};
    InstallOrderedManagerChain(transport, kWorld, kManager);
    InstallOrderedTable(
        transport,
        kManager,
        kRecordsBase,
        kIdArray,
        34);

    ExecutionBreakpointRecord nonShape =
        Record(1, kBreakpoint, kRecordsBase);
    nonShape.x20 = 0;
    transport.AddBatch({nonShape}, kBreakpoint);
    transport.AddBatch(
        {Record(2, kBreakpoint, kRecordsBase)},
        kBreakpoint);

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(!runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.AcceptedSampleCount() == 1);
    REQUIRE(runtime.RecordsBase() == 0);
    REQUIRE(transport.ReadCount(
                kRecordsBase, sizeof(std::uint64_t)) == 1);

    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.AcceptedSampleCount() == 2);
    REQUIRE(runtime.RecordsBase() == kRecordsBase);
    REQUIRE(runtime.Stop());
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
    constexpr std::uint32_t kRawCount = 16;
    constexpr std::size_t kCount =
        static_cast<std::size_t>(kRawCount);

    std::vector<std::uint8_t> records(kCount * kRecordStride);
    std::vector<std::uint32_t> ids(kCount);
    ids[0] = 101;
    SetCoordinate(records, 0, 100.0f, 200.0f, 3.0f);
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
    ids[6] = 107;
    SetCoordinate(records, 6, 1.0f, 10.0f, 10.0f);
    ids[7] = 108;
    SetCoordinate(records, 7, 1000001.0f, 200.0f, 3.0f);
    ids[8] = 109;
    SetCoordinate(records, 8, 300.0f, 400.0f, 500.0f);
    ids[9] = 110;
    SetCoordinate(records, 9, 300.004f, 400.004f, 500.004f);
    ids[10] = 111;
    SetCoordinate(records, 10, 0.0001f, -0.0002f, 0.0003f);
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
    auto callbacks = transport.Callbacks();
    callbacks.readRecords = {};
    callbacks.readCandidate =
        [&](std::uintptr_t& candidate) {
            candidate = kRecordsBase;
            return HardwareBreakpointCandidateSampleResult::Accepted;
        };

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        callbacks,
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(transport.ReadCount(
                kRecordsBase,
                kCount * kRecordStride) == 2);
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
    REQUIRE(runtime.PublishedCoordinateCount() == 10);

    HardwareBreakpointCoordinate coordinate{};
    REQUIRE(runtime.Lookup(101, kWorld, coordinate));
    REQUIRE(coordinate.x == 100.0f);
    REQUIRE(coordinate.y == 200.0f);
    REQUIRE(coordinate.z == 83.0f);
    REQUIRE(runtime.Lookup(102, kWorld, coordinate));
    REQUIRE(runtime.Lookup(103, kWorld, coordinate));
    REQUIRE(runtime.Lookup(104, kWorld, coordinate));
    REQUIRE(!runtime.Lookup(105, kWorld, coordinate));
    REQUIRE(!runtime.Lookup(106, kWorld, coordinate));
    REQUIRE(runtime.Lookup(107, kWorld, coordinate));
    REQUIRE(coordinate.x == 1.0f);
    REQUIRE(coordinate.y == 10.0f);
    REQUIRE(coordinate.z == 90.0f);
    REQUIRE(runtime.Lookup(108, kWorld, coordinate));
    REQUIRE(coordinate.x == 1000001.0f);
    REQUIRE(coordinate.y == 200.0f);
    REQUIRE(coordinate.z == 83.0f);
    REQUIRE(runtime.Lookup(109, kWorld, coordinate));
    REQUIRE(coordinate.x == 300.0f);
    REQUIRE(coordinate.y == 400.0f);
    REQUIRE(coordinate.z == 580.0f);
    REQUIRE(runtime.Lookup(110, kWorld, coordinate));
    REQUIRE(coordinate.x == 300.004f);
    REQUIRE(coordinate.y == 400.004f);
    REQUIRE(coordinate.z == 580.004f);
    REQUIRE(runtime.Lookup(111, kWorld, coordinate));
    REQUIRE(coordinate.x == 0.0001f);
    REQUIRE(coordinate.y == -0.0002f);
    REQUIRE(coordinate.z > 80.0f);
    REQUIRE(coordinate.z < 80.001f);
    REQUIRE(runtime.Lookup(199, kWorld, coordinate));
    REQUIRE(coordinate.x == 7.0f);
    REQUIRE(coordinate.z == 89.0f);

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
    REQUIRE(runtime.PublishedCoordinateCount() == 0);
    REQUIRE(!runtime.Lookup(101, kWorld, coordinate));

    std::fill(ids.begin(), ids.end(), std::uint32_t{0});
    ids[0] = 201;
    SetCoordinate(records, 0, 9.0f, 0.0f, 0.0f);
    InstallOrderedTable(
        transport,
        kManager,
        kRecordsBase,
        kIdArray,
        kRawCount,
        &records,
        &ids);
    transport.AddBatch(
        {Record(3, kBreakpoint, kRecordsBase)},
        kBreakpoint);
    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.PublishedCoordinateCount() == 1);
    REQUIRE(!runtime.Lookup(101, kWorld, coordinate));
    REQUIRE(runtime.Lookup(201, kWorld, coordinate));
    REQUIRE(coordinate.x == 9.0f);
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
        34);
    transport.AddBatch(
        {Record(1, kBreakpoint, recordsBase)},
        kBreakpoint);
    transport.AddBatch(
        {Record(2, kBreakpoint, recordsBase)},
        kBreakpoint);

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(!runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.Poll(kWorld, kManager) == expectedResult);
    REQUIRE(runtime.AcceptedSampleCount() ==
            (expectedResult ? 2U : 0U));
    REQUIRE(runtime.RecordsBase() ==
            (expectedResult ? recordsBase : 0U));
    REQUIRE(transport.ReadCount(
                recordsBase, sizeof(std::uint64_t)) ==
            (expectedResult ? 2U : 0U));
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
        34);
    transport.AddBatch(
        {Record(1, kBreakpoint, kRecordsBase)},
        kBreakpoint);
    transport.AddBatch(
        {Record(2, kBreakpoint, kRecordsBase)},
        kBreakpoint);

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(!runtime.Poll(kWorld));
    REQUIRE(runtime.Poll(kWorld) == expectedResult);
    REQUIRE(runtime.AcceptedSampleCount() == 2);
    REQUIRE(runtime.RecordsBase() ==
            (expectedResult ? kRecordsBase : 0U));
    REQUIRE(transport.ReadCount(
                manager + kIdArrayOffset,
                sizeof(std::uintptr_t)) ==
            (expectedResult ? 4U : 0U));
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
    constexpr std::size_t kCount = 34;

    FakeRuntimeTransport transport{};
    InstallOrderedManagerChain(transport, kWorld, kManager);
    InstallOrderedTable(
        transport,
        kManager,
        kRecordsBase,
        idArray,
        34);
    transport.AddBatch(
        {Record(1, kBreakpoint, kRecordsBase)},
        kBreakpoint);
    transport.AddBatch(
        {Record(2, kBreakpoint, kRecordsBase)},
        kBreakpoint);

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(!runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.Poll(kWorld, kManager) == expectedResult);
    REQUIRE(transport.ReadCount(
                idArray,
                kCount * sizeof(std::uint32_t)) ==
            (expectedResult ? 4U : 0U));
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

void TestOrderedReadFailureAndReconfigureClearsState() {
    constexpr std::uintptr_t kBreakpoint = 0x1E000;
    constexpr std::uintptr_t kSecondBreakpoint = 0x1F000;
    constexpr std::uintptr_t kWorld = 0x20000;
    constexpr std::uintptr_t kManager =
        UINT64_C(0x6005010000);
    constexpr std::uintptr_t kRecordsA =
        UINT64_C(0x6005100000);
    constexpr std::uintptr_t kIdArray =
        UINT64_C(0x6005300000);

    FakeRuntimeTransport transport{};
    InstallOrderedManagerChain(transport, kWorld, kManager);
    InstallOrderedTable(
        transport,
        kManager,
        kRecordsA,
        kIdArray,
        34);
    transport.AddBatch(
        {Record(1, kBreakpoint, kRecordsA)},
        kBreakpoint);
    transport.AddBatch(
        {Record(2, kBreakpoint, kRecordsA)},
        kBreakpoint);

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(!runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.RecordsBase() == kRecordsA);
    REQUIRE(runtime.AcceptedSampleCount() == 2);
    REQUIRE(runtime.PublishedCoordinateCount() == 32);

    HardwareBreakpointCoordinate coordinate{};
    REQUIRE(runtime.Lookup(1000, kWorld, coordinate));
    REQUIRE(coordinate.x == 100.0f);
    REQUIRE(coordinate.z == 380.0f);

    REQUIRE(!runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.NeedsReconfigure());
    REQUIRE(runtime.RecordsBase() == kRecordsA);
    REQUIRE(runtime.AcceptedSampleCount() == 2);
    REQUIRE(runtime.PublishedCoordinateCount() == 32);
    REQUIRE(runtime.Lookup(1000, kWorld, coordinate));
    REQUIRE(coordinate.x == 100.0f);

    REQUIRE(runtime.Reconfigure(
        kSecondBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(!runtime.NeedsReconfigure());
    REQUIRE(runtime.BreakpointAddress() == kSecondBreakpoint);
    REQUIRE(transport.configuredAddresses.size() == 2);
    REQUIRE(transport.configuredAddresses.back() ==
            kSecondBreakpoint);
    REQUIRE(transport.removeCount == 1);
    REQUIRE(runtime.RecordsBase() == 0);
    REQUIRE(runtime.AcceptedSampleCount() == 0);
    REQUIRE(runtime.PublishedCoordinateCount() == 0);
    REQUIRE(!runtime.Lookup(1000, kWorld, coordinate));

    transport.AddBatch(
        {Record(1, kSecondBreakpoint, kRecordsA)},
        kSecondBreakpoint);
    transport.AddBatch(
        {Record(2, kSecondBreakpoint, kRecordsA)},
        kSecondBreakpoint);
    REQUIRE(!runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.RecordsBase() == kRecordsA);
    REQUIRE(runtime.AcceptedSampleCount() == 2);
    REQUIRE(runtime.Lookup(1000, kWorld, coordinate));
    REQUIRE(coordinate.x == 100.0f);
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
        34);
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
    REQUIRE(!runtime.Poll(kWorld, kManager));
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
        kSecondBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(runtime.NeedsReconfigure());
    REQUIRE(runtime.BreakpointAddress() == kBreakpoint);
    REQUIRE(transport.configuredAddresses.size() == 1);
    REQUIRE(transport.removeCount == 1);

    transport.removeResult = true;
    REQUIRE(runtime.Reconfigure(
        kSecondBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(!runtime.NeedsReconfigure());
    REQUIRE(runtime.BreakpointAddress() == kSecondBreakpoint);
    REQUIRE(transport.configuredAddresses.size() == 2);
    REQUIRE(transport.removeCount == 2);
    REQUIRE(runtime.Stop());
    REQUIRE(transport.removeCount == 3);
}

void TestOrderedManagerChainPolicy() {
    constexpr std::uintptr_t kBreakpoint = 0x25000;
    constexpr std::uintptr_t kWorld = 0x26000;
    constexpr std::uintptr_t kManager =
        UINT64_C(0x6007010000);
    constexpr std::uintptr_t kTaggedManager =
        UINT64_C(0xB400000000000000) | kManager;
    constexpr std::uintptr_t kRecordsBase =
        UINT64_C(0x6007100000);
    constexpr std::uintptr_t kIdArray =
        UINT64_C(0x6007300000);

    {
        FakeRuntimeTransport transport{};
        transport.PutValue(kWorld + kManagerOffset, kManager);
        InstallOrderedTable(
            transport,
            kManager,
            kRecordsBase,
            kIdArray,
            34);
        transport.AddBatch(
            {Record(1, kBreakpoint, kRecordsBase)},
            kBreakpoint);

        HardwareBreakpointCoordinateRuntime runtime;
        REQUIRE(runtime.Start(
            kBreakpoint,
            transport.Callbacks(),
            HardwareBreakpointCoordinateProfile::OrderedRecordTable));
        REQUIRE(!runtime.Poll(kWorld));
        REQUIRE(runtime.AcceptedSampleCount() == 1);
        REQUIRE(runtime.RecordsBase() == 0);
        REQUIRE(transport.ReadCount(
                    kWorld + kManagerOffset,
                    sizeof(std::uintptr_t)) == 0);
        REQUIRE(runtime.Stop());
    }

    {
        FakeRuntimeTransport transport{};
        InstallOrderedManagerChain(
            transport, kWorld, kTaggedManager);
        InstallOrderedTable(
            transport,
            kManager,
            kRecordsBase,
            kIdArray,
            34);
        transport.AddBatch(
            {Record(1, kBreakpoint, kRecordsBase)},
            kBreakpoint);
        transport.AddBatch(
            {Record(2, kBreakpoint, kRecordsBase)},
            kBreakpoint);

        HardwareBreakpointCoordinateRuntime runtime;
        REQUIRE(runtime.Start(
            kBreakpoint,
            transport.Callbacks(),
            HardwareBreakpointCoordinateProfile::OrderedRecordTable));
        REQUIRE(!runtime.Poll(kWorld, kManager));
        REQUIRE(runtime.Poll(kWorld, kManager));
        REQUIRE(transport.ReadCount(
                    kManager + kIdArrayOffset,
                    sizeof(std::uintptr_t)) == 4);
        REQUIRE(runtime.Stop());
    }
}

void TestOrderedSeparateWorldAndTargetRoot() {
    constexpr std::uintptr_t kBreakpoint = 0x27000;
    constexpr std::uintptr_t kWorld = 0x28000;
    constexpr std::uintptr_t kNextWorld = 0x29000;
    constexpr std::uintptr_t kTargetRoot =
        UINT64_C(0x6008010000);
    constexpr std::uintptr_t kManager =
        UINT64_C(0x6009010000);
    constexpr std::uintptr_t kRecordsBase =
        UINT64_C(0x6009100000);
    constexpr std::uintptr_t kIdArray =
        UINT64_C(0x6009300000);
    constexpr std::uint32_t kRawCount = 16;
    constexpr std::size_t kCount =
        static_cast<std::size_t>(kRawCount);

    std::vector<std::uint8_t> records(kCount * kRecordStride);
    std::vector<std::uint32_t> ids(kCount);
    ids[0] = 501;
    SetCoordinate(records, 0, 110.0f, 120.0f, 13.0f);

    FakeRuntimeTransport transport{};
    InstallOrderedManagerChain(transport, kTargetRoot, kManager);
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
    auto callbacks = transport.Callbacks();
    callbacks.readRecords = {};
    callbacks.readCandidate =
        [&](std::uintptr_t& candidate) {
            candidate = kRecordsBase;
            return HardwareBreakpointCandidateSampleResult::Accepted;
        };

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        callbacks,
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(runtime.Poll(kWorld, kTargetRoot, kManager));
    REQUIRE(transport.ReadCount(
                kTargetRoot + kTargetLinkOffset,
                sizeof(std::uintptr_t)) == 2);
    REQUIRE(transport.ReadCount(
                kWorld + kTargetLinkOffset,
                sizeof(std::uintptr_t)) == 0);

    HardwareBreakpointCoordinate coordinate{};
    REQUIRE(runtime.Lookup(501, kWorld, coordinate));
    REQUIRE(coordinate.x == 110.0f);
    REQUIRE(coordinate.y == 120.0f);
    REQUIRE(coordinate.z == 93.0f);

    transport.AddBatch(
        {Record(2, kBreakpoint, kRecordsBase)},
        kBreakpoint);
    REQUIRE(runtime.Poll(kNextWorld, kTargetRoot, kManager));
    REQUIRE(!runtime.Lookup(501, kWorld, coordinate));
    REQUIRE(runtime.Lookup(501, kNextWorld, coordinate));
    REQUIRE(transport.ReadCount(
                kTargetRoot + kTargetLinkOffset,
                sizeof(std::uintptr_t)) == 4);
    REQUIRE(runtime.Stop());
}

void TestOrderedFallbackQualitySelectionFairness() {
    constexpr std::uintptr_t kBreakpoint = 0x2C000;
    constexpr std::uintptr_t kWorld = 0x2D000;
    constexpr std::uintptr_t kManager =
        UINT64_C(0x6012010000);
    constexpr std::uintptr_t kBadA =
        UINT64_C(0x6012100000);
    constexpr std::uintptr_t kBadB =
        UINT64_C(0x6012200000);
    constexpr std::uintptr_t kBadC =
        UINT64_C(0x6012300000);
    constexpr std::uintptr_t kGood =
        UINT64_C(0x6012400000);
    constexpr std::uintptr_t kIdArray =
        UINT64_C(0x6012500000);
    constexpr std::size_t kItemCount = 33;

    auto ids = MakeSequentialIds(kItemCount, 2000);
    auto good = MakeCoordinateRecords(kItemCount, 0, 32, 100.0f);
    std::vector<std::uint8_t> badA(kItemCount * kRecordStride);
    std::vector<std::uint8_t> badB(kItemCount * kRecordStride);
    std::vector<std::uint8_t> badC(kItemCount * kRecordStride);
    for (std::size_t index = 0; index < 32; ++index) {
        const float tiny = std::numeric_limits<float>::denorm_min();
        SetCoordinate(badA, index, tiny, tiny, tiny);
        SetCoordinate(
            badB, index, 2000000.0f, 3000000.0f, 4000000.0f);
        SetCoordinate(badC, index, 100.0f, 200.0f, 300.0f);
    }

    FakeRuntimeTransport transport{};
    InstallOrderedManagerChain(transport, kWorld, kManager);
    InstallOrderedTable(
        transport,
        kManager,
        kGood,
        kIdArray,
        33,
        &good,
        &ids);
    transport.PutBytes(kBadA, badA.data(), badA.size());
    transport.PutBytes(kBadB, badB.data(), badB.size());
    transport.PutBytes(kBadC, badC.data(), badC.size());

    const auto addBatch = [&](std::uint64_t hitCount) {
        std::vector<ExecutionBreakpointRecord> records;
        for (pid_t tid = 100; tid < 104; ++tid) {
            auto record =
                Record(hitCount, kBreakpoint, kBadA, tid);
            record.x20 = 0;
            records.push_back(record);
        }
        auto badBRecord =
            Record(hitCount, kBreakpoint, kBadB, 200);
        badBRecord.x20 = 0;
        records.push_back(badBRecord);
        auto badCRecord =
            Record(hitCount, kBreakpoint, kBadC, 300);
        badCRecord.x20 = 0;
        records.push_back(badCRecord);
        records.push_back(
            Record(hitCount, kBreakpoint, kGood, 400));
        transport.AddBatch(
            std::move(records), kBreakpoint);
    };
    addBatch(1);
    addBatch(2);
    addBatch(3);

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(!runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.RecordsBase() == 0);
    REQUIRE(!runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.RecordsBase() == 0);
    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.RecordsBase() == kGood);
    REQUIRE(runtime.PublishedCoordinateCount() == 32);
    HardwareBreakpointCoordinate coordinate{};
    REQUIRE(runtime.Lookup(2000, kWorld, coordinate));
    REQUIRE(coordinate.x == 100.0f);
    REQUIRE(runtime.Stop());
}

void TestOrderedFallbackSnapshotCoherenceAndRetry() {
    constexpr std::uintptr_t kBreakpoint = 0x2E000;
    constexpr std::uintptr_t kWorld = 0x2F000;
    constexpr std::uintptr_t kManager =
        UINT64_C(0x6013010000);
    constexpr std::uintptr_t kRecordsBase =
        UINT64_C(0x6013100000);
    constexpr std::uintptr_t kIdArray =
        UINT64_C(0x6013300000);
    constexpr std::size_t kItemCount = 33;
    constexpr std::size_t kBulkSize =
        kItemCount * kRecordStride;

    auto ids = MakeSequentialIds(kItemCount, 3000);
    auto first = MakeCoordinateRecords(kItemCount, 0, 32, 100.0f);
    auto second = MakeCoordinateRecords(kItemCount, 0, 32, 1100.0f);

    FakeRuntimeTransport transport{};
    InstallOrderedManagerChain(transport, kWorld, kManager);
    InstallOrderedTable(
        transport,
        kManager,
        kRecordsBase,
        kIdArray,
        33,
        &first,
        &ids);
    std::size_t bulkReads = 0;
    transport.beforeMemoryRead =
        [&](std::uintptr_t address, std::size_t size) {
            if (address != kRecordsBase || size != kBulkSize) {
                return;
            }
            ++bulkReads;
            if (bulkReads == 2) {
                transport.PutBytes(
                    kRecordsBase, second.data(), second.size());
            }
        };
    const ExecutionBreakpointRecord sample =
        Record(1, kBreakpoint, kRecordsBase);
    transport.AddBatch({sample}, kBreakpoint);
    transport.AddBatch({sample}, kBreakpoint);
    transport.AddBatch({sample}, kBreakpoint);

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(!runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.RecordsBase() == 0);
    transport.beforeMemoryRead = {};
    REQUIRE(!runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.RecordsBase() == 0);
    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.AcceptedSampleCount() == 1);
    REQUIRE(runtime.RecordsBase() == kRecordsBase);
    HardwareBreakpointCoordinate coordinate{};
    REQUIRE(runtime.Lookup(3000, kWorld, coordinate));
    REQUIRE(coordinate.x == 1100.0f);
    REQUIRE(runtime.Stop());
}

void TestOrderedFallbackFiltersUnstableRow() {
    constexpr std::uintptr_t kBreakpoint = 0x30000;
    constexpr std::uintptr_t kWorld = 0x31000;
    constexpr std::uintptr_t kManager =
        UINT64_C(0x6014010000);
    constexpr std::uintptr_t kRecordsBase =
        UINT64_C(0x6014100000);
    constexpr std::uintptr_t kIdArray =
        UINT64_C(0x6014300000);
    constexpr std::size_t kItemCount = 33;
    constexpr std::size_t kBulkSize =
        kItemCount * kRecordStride;

    auto ids = MakeSequentialIds(kItemCount, 4000);
    auto records = MakeCoordinateRecords(
        kItemCount, 0, 32, 100.0f);
    auto changed = records;
    SetCoordinate(changed, 0, 2100.0f, 2200.0f, 2300.0f);

    FakeRuntimeTransport transport{};
    InstallOrderedManagerChain(transport, kWorld, kManager);
    InstallOrderedTable(
        transport,
        kManager,
        kRecordsBase,
        kIdArray,
        33,
        &records,
        &ids);
    std::size_t bulkReads = 0;
    transport.beforeMemoryRead =
        [&](std::uintptr_t address, std::size_t size) {
            if (address != kRecordsBase || size != kBulkSize) {
                return;
            }
            ++bulkReads;
            if (bulkReads == 4) {
                transport.PutBytes(
                    kRecordsBase, changed.data(), changed.size());
            }
        };
    transport.AddBatch(
        {Record(1, kBreakpoint, kRecordsBase)},
        kBreakpoint);
    transport.AddBatch(
        {Record(2, kBreakpoint, kRecordsBase)},
        kBreakpoint);

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(!runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.PublishedCoordinateCount() == 31);
    HardwareBreakpointCoordinate coordinate{};
    REQUIRE(!runtime.Lookup(4000, kWorld, coordinate));
    REQUIRE(runtime.Lookup(4001, kWorld, coordinate));
    REQUIRE(runtime.Stop());
}

void TestOrderedFallbackPreferredBaseMigration() {
    constexpr std::uintptr_t kBreakpoint = 0x32000;
    constexpr std::uintptr_t kWorld = 0x33000;
    constexpr std::uintptr_t kManager =
        UINT64_C(0x6015010000);
    constexpr std::uintptr_t kRecordsA =
        UINT64_C(0x6015100000);
    constexpr std::uintptr_t kRecordsB =
        UINT64_C(0x6015200000);
    constexpr std::uintptr_t kIdArray =
        UINT64_C(0x6015300000);
    constexpr std::size_t kItemCount = 33;

    auto ids = MakeSequentialIds(kItemCount, 5000);
    auto recordsA = MakeCoordinateRecords(
        kItemCount, 0, 16, 100.0f);
    auto recordsB = MakeCoordinateRecords(
        kItemCount, 16, 16, 1000.0f);

    FakeRuntimeTransport transport{};
    InstallOrderedManagerChain(transport, kWorld, kManager);
    InstallOrderedTable(
        transport,
        kManager,
        kRecordsA,
        kIdArray,
        33,
        &recordsA,
        &ids);
    transport.PutBytes(
        kRecordsB, recordsB.data(), recordsB.size());
    transport.AddBatch(
        {Record(1, kBreakpoint, kRecordsA)},
        kBreakpoint);
    transport.AddBatch(
        {Record(2, kBreakpoint, kRecordsA)},
        kBreakpoint);
    const auto addMixedBatch = [&](std::uint64_t hitCount) {
        std::vector<ExecutionBreakpointRecord> records;
        for (pid_t tid = 600; tid < 606; ++tid) {
            records.push_back(
                Record(hitCount, kBreakpoint, kRecordsB, tid));
        }
        records.push_back(
            Record(hitCount, kBreakpoint, kRecordsA, 700));
        transport.AddBatch(
            std::move(records), kBreakpoint);
    };
    addMixedBatch(3);
    addMixedBatch(4);

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(!runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.RecordsBase() == kRecordsA);
    REQUIRE(runtime.PublishedCoordinateCount() == 16);
    HardwareBreakpointCoordinate coordinate{};
    REQUIRE(runtime.Lookup(5000, kWorld, coordinate));
    REQUIRE(!runtime.Lookup(5016, kWorld, coordinate));

    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.RecordsBase() == kRecordsA);
    REQUIRE(runtime.PublishedCoordinateCount() == 16);
    REQUIRE(runtime.Lookup(5000, kWorld, coordinate));

    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.RecordsBase() == kRecordsB);
    REQUIRE(runtime.PublishedCoordinateCount() == 16);
    REQUIRE(!runtime.Lookup(5000, kWorld, coordinate));
    REQUIRE(runtime.Lookup(5016, kWorld, coordinate));
    REQUIRE(coordinate.x == 1000.0f);
    REQUIRE(runtime.Stop());
}

void TestOrderedFallbackFailureSwitchAndExpiry() {
    constexpr std::uintptr_t kBreakpoint = 0x34000;
    constexpr std::uintptr_t kWorld = 0x35000;
    constexpr std::uintptr_t kManager =
        UINT64_C(0x6016010000);
    constexpr std::uintptr_t kRecordsA =
        UINT64_C(0x6016100000);
    constexpr std::uintptr_t kRecordsB =
        UINT64_C(0x6016200000);
    constexpr std::uintptr_t kIdArray =
        UINT64_C(0x6016300000);
    constexpr std::size_t kItemCount = 33;

    auto ids = MakeSequentialIds(kItemCount, 6000);
    auto recordsA = MakeCoordinateRecords(
        kItemCount, 0, 16, 100.0f);
    auto recordsB = MakeCoordinateRecords(
        kItemCount, 16, 16, 1000.0f);

    {
        FakeRuntimeTransport transport{};
        InstallOrderedManagerChain(transport, kWorld, kManager);
        InstallOrderedTable(
            transport,
            kManager,
            kRecordsA,
            kIdArray,
            33,
            &recordsA,
            &ids);
        transport.PutBytes(
            kRecordsB, recordsB.data(), recordsB.size());
        transport.AddBatch(
            {Record(1, kBreakpoint, kRecordsA)},
            kBreakpoint);
        transport.AddBatch(
            {Record(2, kBreakpoint, kRecordsA)},
            kBreakpoint);
        transport.AddBatch(
            {Record(3, kBreakpoint, kRecordsB)},
            kBreakpoint);
        transport.AddBatch(
            {Record(4, kBreakpoint, kRecordsB)},
            kBreakpoint);
        transport.AddBatch(
            {Record(5, kBreakpoint, kRecordsB)},
            kBreakpoint);

        HardwareBreakpointCoordinateRuntime runtime;
        REQUIRE(runtime.Start(
            kBreakpoint,
            transport.Callbacks(),
            HardwareBreakpointCoordinateProfile::OrderedRecordTable));
        REQUIRE(!runtime.Poll(kWorld, kManager));
        REQUIRE(runtime.Poll(kWorld, kManager));
        transport.failingAddress = kRecordsA;

        HardwareBreakpointCoordinate coordinate{};
        REQUIRE(!runtime.Poll(kWorld, kManager));
        REQUIRE(runtime.RecordsBase() == kRecordsA);
        REQUIRE(runtime.PublishedCoordinateCount() == 16);
        REQUIRE(runtime.Lookup(6000, kWorld, coordinate));
        REQUIRE(!runtime.Poll(kWorld, kManager));
        REQUIRE(runtime.RecordsBase() == kRecordsA);
        REQUIRE(runtime.PublishedCoordinateCount() == 16);
        REQUIRE(runtime.Lookup(6000, kWorld, coordinate));
        REQUIRE(runtime.Poll(kWorld, kManager));
        REQUIRE(runtime.RecordsBase() == kRecordsB);
        REQUIRE(runtime.PublishedCoordinateCount() == 16);
        REQUIRE(!runtime.Lookup(6000, kWorld, coordinate));
        REQUIRE(runtime.Lookup(6016, kWorld, coordinate));
        REQUIRE(runtime.Stop());
    }

    {
        FakeRuntimeTransport transport{};
        InstallOrderedManagerChain(transport, kWorld, kManager);
        InstallOrderedTable(
            transport,
            kManager,
            kRecordsA,
            kIdArray,
            33,
            &recordsA,
            &ids);
        const ExecutionBreakpointRecord sample =
            Record(1, kBreakpoint, kRecordsA);
        transport.AddBatch({sample}, kBreakpoint);
        transport.AddBatch(
            {Record(2, kBreakpoint, kRecordsA)},
            kBreakpoint);

        HardwareBreakpointCoordinateRuntime runtime;
        REQUIRE(runtime.Start(
            kBreakpoint,
            transport.Callbacks(),
            HardwareBreakpointCoordinateProfile::OrderedRecordTable));
        REQUIRE(!runtime.Poll(kWorld, kManager));
        REQUIRE(runtime.Poll(kWorld, kManager));
        REQUIRE(!runtime.Poll(kWorld, kManager));
        REQUIRE(runtime.PublishedCoordinateCount() == 16);
        REQUIRE(!runtime.Poll(kWorld, kManager));
        REQUIRE(runtime.PublishedCoordinateCount() == 16);
        REQUIRE(!runtime.Poll(kWorld, kManager));
        REQUIRE(runtime.RecordsBase() == 0);
        REQUIRE(runtime.PublishedCoordinateCount() == 0);

        transport.AddBatch(
            {Record(2, kBreakpoint, kRecordsA)},
            kBreakpoint);
        transport.AddBatch(
            {Record(2, kBreakpoint, kRecordsA)},
            kBreakpoint);
        REQUIRE(!runtime.Poll(kWorld, kManager));
        REQUIRE(runtime.Poll(kWorld, kManager));
        REQUIRE(runtime.RecordsBase() == kRecordsA);
        REQUIRE(runtime.PublishedCoordinateCount() == 16);
        REQUIRE(runtime.Stop());
    }
}

void TestOrderedFallbackAtomicReplacement() {
    constexpr std::uintptr_t kBreakpoint = 0x36000;
    constexpr std::uintptr_t kWorld = 0x37000;
    constexpr std::uintptr_t kManager =
        UINT64_C(0x6017010000);
    constexpr std::uintptr_t kRecordsBase =
        UINT64_C(0x6017100000);
    constexpr std::uintptr_t kIdArray =
        UINT64_C(0x6017300000);
    constexpr std::size_t kItemCount = 33;

    auto ids = MakeSequentialIds(kItemCount, 7000);
    auto records = MakeCoordinateRecords(
        kItemCount, 0, 32, 100.0f);

    FakeRuntimeTransport transport{};
    InstallOrderedManagerChain(transport, kWorld, kManager);
    InstallOrderedTable(
        transport,
        kManager,
        kRecordsBase,
        kIdArray,
        33,
        &records,
        &ids);
    for (std::uint64_t hitCount = 1; hitCount <= 12; ++hitCount) {
        transport.AddBatch(
            {Record(hitCount, kBreakpoint, kRecordsBase)},
            kBreakpoint);
    }

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(!runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.Poll(kWorld, kManager));
    HardwareBreakpointCoordinate coordinate{};
    REQUIRE(runtime.Lookup(7000, kWorld, coordinate));
    REQUIRE(runtime.Lookup(7001, kWorld, coordinate));

    ids[1] = 0;
    transport.PutBytes(
        kIdArray,
        ids.data(),
        ids.size() * sizeof(std::uint32_t));
    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(!runtime.Lookup(7001, kWorld, coordinate));
    REQUIRE(runtime.Lookup(7000, kWorld, coordinate));

    SetCoordinate(records, 0, 0.0f, 0.0f, 0.0f);
    transport.PutBytes(
        kRecordsBase, records.data(), records.size());
    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(!runtime.Lookup(7000, kWorld, coordinate));

    SetCoordinate(records, 0, 100.0f, 200.0f, 300.0f);
    transport.PutBytes(
        kRecordsBase, records.data(), records.size());
    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.Lookup(7000, kWorld, coordinate));

    SetCoordinate(records, 2, 2200.0f, 2300.0f, 2400.0f);
    transport.PutBytes(
        kRecordsBase, records.data(), records.size());
    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.Lookup(7002, kWorld, coordinate));
    REQUIRE(coordinate.x == 2200.0f);
    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.Lookup(7002, kWorld, coordinate));
    REQUIRE(coordinate.x == 2200.0f);

    SetCoordinate(records, 2, 120.0f, 220.0f, 320.0f);
    transport.PutBytes(
        kRecordsBase, records.data(), records.size());
    REQUIRE(runtime.Poll(kWorld, kManager));
    REQUIRE(runtime.Lookup(7002, kWorld, coordinate));
    REQUIRE(coordinate.x == 120.0f);
    REQUIRE(runtime.Stop());
}

void TestOrderedGenerationChangeClearsState() {
    constexpr std::uintptr_t kBreakpoint = 0x38000;
    constexpr std::uintptr_t kWorld = 0x39000;
    constexpr std::uintptr_t kTargetRoot =
        UINT64_C(0x6018010000);
    constexpr std::uintptr_t kManagerA =
        UINT64_C(0x6019010000);
    constexpr std::uintptr_t kManagerB =
        UINT64_C(0x601A010000);
    constexpr std::uintptr_t kRecordsA =
        UINT64_C(0x6019100000);
    constexpr std::uintptr_t kRecordsB =
        UINT64_C(0x601A100000);
    constexpr std::uintptr_t kIdArrayA =
        UINT64_C(0x6019300000);
    constexpr std::uintptr_t kIdArrayB =
        UINT64_C(0x601A300000);

    FakeRuntimeTransport transport{};
    InstallOrderedManagerChain(
        transport, kTargetRoot, kManagerA);
    InstallOrderedTable(
        transport,
        kManagerA,
        kRecordsA,
        kIdArrayA,
        34);
    transport.AddBatch(
        {Record(1, kBreakpoint, kRecordsA)},
        kBreakpoint);
    transport.AddBatch(
        {Record(2, kBreakpoint, kRecordsA)},
        kBreakpoint);

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        transport.Callbacks(),
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(!runtime.Poll(kWorld, kTargetRoot, kManagerA));
    REQUIRE(runtime.Poll(kWorld, kTargetRoot, kManagerA));
    HardwareBreakpointCoordinate coordinate{};
    REQUIRE(runtime.Lookup(1000, kWorld, coordinate));

    InstallOrderedManagerChain(
        transport, kTargetRoot, kManagerB);
    InstallOrderedTable(
        transport,
        kManagerB,
        kRecordsB,
        kIdArrayB,
        34);
    transport.AddBatch(
        {Record(3, kBreakpoint, kRecordsB)},
        kBreakpoint);
    REQUIRE(!runtime.Poll(kWorld, kTargetRoot, kManagerA));
    REQUIRE(runtime.RecordsBase() == 0);
    REQUIRE(runtime.PublishedCoordinateCount() == 0);
    REQUIRE(!runtime.Lookup(1000, kWorld, coordinate));
    REQUIRE(runtime.Stop());
}

void TestOrderedExplicitCandidateLifecycle() {
    constexpr std::uintptr_t kBreakpoint = 0x2A000;
    constexpr std::uintptr_t kWorld = 0x2B000;
    constexpr std::uintptr_t kTargetRoot =
        UINT64_C(0x6010010000);
    constexpr std::uintptr_t kManager =
        UINT64_C(0x6011010000);
    constexpr std::uintptr_t kRecordsBase =
        UINT64_C(0x6011100000);
    constexpr std::uintptr_t kIdArray =
        UINT64_C(0x6011300000);
    constexpr std::uint32_t kRawCount = 16;
    constexpr std::size_t kCount =
        static_cast<std::size_t>(kRawCount);

    std::vector<std::uint8_t> records(kCount * kRecordStride);
    std::vector<std::uint32_t> ids(kCount);
    ids[0] = 801;
    SetCoordinate(records, 0, 100.0f, 200.0f, 30.0f);

    FakeRuntimeTransport transport{};
    InstallOrderedManagerChain(transport, kTargetRoot, kManager);
    InstallOrderedTable(
        transport,
        kManager,
        kRecordsBase,
        kIdArray,
        kRawCount,
        &records,
        &ids);
    std::vector<HardwareBreakpointCandidateSampleResult> results{
        HardwareBreakpointCandidateSampleResult::Accepted,
        HardwareBreakpointCandidateSampleResult::Retry,
        HardwareBreakpointCandidateSampleResult::Reset,
        HardwareBreakpointCandidateSampleResult::Accepted,
    };
    std::size_t nextResult = 0;
    auto callbacks = transport.Callbacks();
    callbacks.readRecords = {};
    callbacks.readCandidate =
        [&](std::uintptr_t& candidate) {
            REQUIRE(nextResult < results.size());
            const auto result = results[nextResult++];
            candidate = result ==
                    HardwareBreakpointCandidateSampleResult::Accepted
                ? kRecordsBase
                : 0;
            return result;
        };

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(
        kBreakpoint,
        callbacks,
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(runtime.Poll(kWorld, kTargetRoot, kManager));
    REQUIRE(runtime.AcceptedSampleCount() == 1);
    REQUIRE(runtime.RecordsBase() == kRecordsBase);
    HardwareBreakpointCoordinate coordinate{};
    REQUIRE(runtime.Lookup(801, kWorld, coordinate));
    REQUIRE(coordinate.x == 100.0f);
    REQUIRE(coordinate.z == 110.0f);

    REQUIRE(runtime.Poll(kWorld, kTargetRoot, kManager));
    REQUIRE(runtime.AcceptedSampleCount() == 1);
    REQUIRE(runtime.Lookup(801, kWorld, coordinate));

    REQUIRE(!runtime.Poll(kWorld, kTargetRoot, kManager));
    REQUIRE(runtime.NeedsReconfigure());
    REQUIRE(runtime.RecordsBase() == 0);
    REQUIRE(!runtime.Lookup(801, kWorld, coordinate));

    REQUIRE(runtime.Reconfigure(
        kBreakpoint,
        callbacks,
        HardwareBreakpointCoordinateProfile::OrderedRecordTable));
    REQUIRE(runtime.Poll(kWorld, kTargetRoot, kManager));
    REQUIRE(runtime.AcceptedSampleCount() == 1);
    REQUIRE(runtime.RecordsBase() == kRecordsBase);
    REQUIRE(runtime.Lookup(801, kWorld, coordinate));
    REQUIRE(runtime.Stop());
}

}  // namespace

void RunHardwareBreakpointCoordinateRuntimeTests() {
    TestStableRecordTableUsesNewestRecordPerThread();
    TestMeshStreamUsesRegisterPairAndRejectsX23Table();
    TestMeshStreamMeshAndIdReuse();
    TestMeshStreamFreshnessAndProfileSwitch();
    TestTablePublicationAndStability();
    TestTenSlotModeAndFirstTie();
    TestCountAndCandidateBoundaries();
    TestOrderedRawCountBoundaries();
    TestOrderedExactSnapshotRejectsRecordRace();
    TestOrderedFallbackDoesNotRequireCursorShape();
    TestOrderedRecordReadAndCoordinateValidation();
    TestOrderedStrictPointerBounds();
    TestOrderedReadFailureAndReconfigureClearsState();
    TestOrderedRecordReorderAndCountRollback();
    TestOrderedReconfigureRequiresRemoval();
    TestOrderedManagerChainPolicy();
    TestOrderedSeparateWorldAndTargetRoot();
    TestOrderedFallbackQualitySelectionFairness();
    TestOrderedFallbackSnapshotCoherenceAndRetry();
    TestOrderedFallbackFiltersUnstableRow();
    TestOrderedFallbackPreferredBaseMigration();
    TestOrderedFallbackFailureSwitchAndExpiry();
    TestOrderedFallbackAtomicReplacement();
    TestOrderedGenerationChangeClearsState();
    TestOrderedExplicitCandidateLifecycle();
}
