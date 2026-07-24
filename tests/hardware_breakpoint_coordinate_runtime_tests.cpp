#if 0

#include "test_support.h"

#include "game/native/HardwareBreakpointCoordinateRuntime.h"

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

constexpr std::uintptr_t kCoordinateValueOffset = 0x80;
constexpr std::uintptr_t kCoordinateIdOffset = 0x2D8;

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
                                 std::uintptr_t x20,
                                 std::uintptr_t x21,
                                 pid_t tid = 123) {
    ExecutionBreakpointRecord record{};
    record.tid = tid;
    record.hitCount = hitCount;
    record.pc = pc;
    record.sp = 0x11110000;
    record.x0 = 0x22220000;
    record.x20 = x20;
    record.x21 = x21;
    record.x23 = 0x33330000;
    return record;
}

void InstallSample(FakeRuntimeTransport& transport,
                   std::uintptr_t coordinateBase,
                   std::uintptr_t mesh,
                   std::uint32_t id,
                   HardwareBreakpointCoordinate coordinate) {
    transport.PutValue(mesh + kCoordinateIdOffset, id);
    transport.PutValue(
        coordinateBase + kCoordinateValueOffset, coordinate);
}

void TestDirectPublicationAndLifecycle() {
    constexpr std::uintptr_t kBreakpoint = 0x4000;
    constexpr std::uintptr_t kSecondBreakpoint = 0x4800;
    constexpr std::uintptr_t kWorld = 0x5000;
    constexpr std::uintptr_t kOtherWorld = 0x6000;
    constexpr std::uintptr_t kCoordinateA = 0x12345000;
    constexpr std::uintptr_t kCoordinateB = 0x12346000;
    constexpr std::uintptr_t kCoordinateC = 0x12347000;
    constexpr std::uintptr_t kMeshA = 0x22345000;
    constexpr std::uintptr_t kMeshB = 0x22346000;
    constexpr std::uintptr_t kMeshC = 0x22347000;
    constexpr std::uintptr_t kTaggedCoordinateA =
        UINT64_C(0xAB00000012345000);
    constexpr std::uintptr_t kTaggedMeshA =
        UINT64_C(0xCD00000022345000);

    FakeRuntimeTransport transport{};
    InstallSample(
        transport,
        kCoordinateA,
        kMeshA,
        101,
        HardwareBreakpointCoordinate{1.0f, 2.0f, 3.0f});
    InstallSample(
        transport,
        kCoordinateB,
        kMeshB,
        202,
        HardwareBreakpointCoordinate{-4.0f, 5.0f, -6.0f});
    InstallSample(
        transport,
        kCoordinateC,
        kMeshC,
        303,
        HardwareBreakpointCoordinate{7.0f, 8.0f, 9.0f});

    const ExecutionBreakpointRecord sampleA =
        Record(1, kBreakpoint, kTaggedCoordinateA, kTaggedMeshA);
    const ExecutionBreakpointRecord sampleB =
        Record(2, kBreakpoint, kCoordinateB, kMeshB);
    transport.AddBatch({sampleA, sampleB}, kBreakpoint);

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(!runtime.Start(0, transport.Callbacks()));
    REQUIRE(!runtime.Start(kBreakpoint + 2, transport.Callbacks()));
    REQUIRE(runtime.Start(kBreakpoint, transport.Callbacks()));
    REQUIRE(runtime.Poll(kWorld));
    REQUIRE(runtime.RecordsBase() == kCoordinateB);
    REQUIRE(runtime.AcceptedSampleCount() == 2);
    REQUIRE(runtime.PublishedCoordinateCount() == 2);
    REQUIRE(transport.ReadCount(
                kMeshA + kCoordinateIdOffset,
                sizeof(std::uint32_t)) == 2);
    REQUIRE(transport.ReadCount(
                kCoordinateA + kCoordinateValueOffset,
                sizeof(HardwareBreakpointCoordinate)) == 1);

    HardwareBreakpointCoordinate coordinate{};
    REQUIRE(runtime.Lookup(101, kWorld, coordinate));
    REQUIRE(coordinate.x == 1.0f);
    REQUIRE(coordinate.y == 2.0f);
    REQUIRE(coordinate.z == 3.0f);
    REQUIRE(runtime.Lookup(101, kMeshA, kWorld, coordinate));
    REQUIRE(!runtime.Lookup(101, kMeshB, kWorld, coordinate));
    REQUIRE(runtime.Lookup(202, kMeshB, kWorld, coordinate));
    REQUIRE(coordinate.x == -4.0f);
    REQUIRE(coordinate.z == -6.0f);

    transport.AddBatch({sampleA, sampleB}, kBreakpoint);
    REQUIRE(runtime.Poll(kWorld));
    REQUIRE(runtime.AcceptedSampleCount() == 2);

    InstallSample(
        transport,
        kCoordinateA,
        kMeshA,
        101,
        HardwareBreakpointCoordinate{10.0f, 20.0f, 30.0f});
    transport.AddBatch(
        {Record(3, kBreakpoint, kCoordinateA, kMeshA)},
        kBreakpoint);
    REQUIRE(runtime.Poll(kWorld));
    REQUIRE(runtime.AcceptedSampleCount() == 3);
    REQUIRE(runtime.Lookup(101, kMeshA, kWorld, coordinate));
    REQUIRE(coordinate.x == 10.0f);
    REQUIRE(coordinate.z == 30.0f);

    InstallSample(
        transport,
        kCoordinateB,
        kMeshB,
        203,
        HardwareBreakpointCoordinate{40.0f, 50.0f, 60.0f});
    transport.AddBatch(
        {Record(4, kBreakpoint, kCoordinateB, kMeshB)},
        kBreakpoint);
    REQUIRE(runtime.Poll(kWorld));
    REQUIRE(!runtime.Lookup(202, kMeshB, kWorld, coordinate));
    REQUIRE(runtime.Lookup(203, kMeshB, kWorld, coordinate));

    InstallSample(
        transport,
        kCoordinateC,
        kMeshC,
        101,
        HardwareBreakpointCoordinate{70.0f, 80.0f, 90.0f});
    transport.AddBatch(
        {Record(5, kBreakpoint, kCoordinateC, kMeshC)},
        kBreakpoint);
    REQUIRE(runtime.Poll(kWorld));
    REQUIRE(!runtime.Lookup(101, kMeshA, kWorld, coordinate));
    REQUIRE(runtime.Lookup(101, kMeshC, kWorld, coordinate));
    REQUIRE(coordinate.x == 70.0f);

    transport.AddBatch(
        {Record(5, kBreakpoint, kCoordinateC, kMeshC)},
        kBreakpoint);
    REQUIRE(runtime.Poll(kOtherWorld, 0xDEADBEEF));
    REQUIRE(!runtime.Lookup(101, kWorld, coordinate));
    REQUIRE(runtime.Lookup(101, kMeshC, kOtherWorld, coordinate));
    REQUIRE(runtime.PublishedCoordinateCount() == 1);

    REQUIRE(runtime.Start(kSecondBreakpoint, transport.Callbacks()));
    REQUIRE(transport.configuredAddresses.back() == kSecondBreakpoint);
    REQUIRE(transport.removeCount == 1);
    REQUIRE(runtime.RecordsBase() == 0);
    REQUIRE(runtime.PublishedCoordinateCount() == 0);
    REQUIRE(runtime.AcceptedSampleCount() == 0);
    REQUIRE(runtime.Stop());
    REQUIRE(transport.removeCount == 2);
}

void TestSampleValidationAndIdentity() {
    constexpr std::uintptr_t kBreakpoint = 0x8000;
    constexpr std::uintptr_t kWorld = 0x9000;
    constexpr std::uintptr_t kCoordinateA = 0x301000;
    constexpr std::uintptr_t kCoordinateB = 0x302000;
    constexpr std::uintptr_t kMeshA = 0x401000;
    constexpr std::uintptr_t kMeshB = 0x402000;

    FakeRuntimeTransport transport{};
    InstallSample(
        transport,
        kCoordinateA,
        kMeshA,
        11,
        HardwareBreakpointCoordinate{1.0f, 2.0f, 3.0f});
    InstallSample(
        transport,
        kCoordinateB,
        kMeshB,
        12,
        HardwareBreakpointCoordinate{4.0f, 5.0f, 6.0f});

    HardwareBreakpointCoordinateRuntime runtime;
    REQUIRE(runtime.Start(kBreakpoint, transport.Callbacks()));

    transport.AddBatch(
        {Record(1, kBreakpoint, kCoordinateA, kMeshA)},
        kBreakpoint);
    REQUIRE(runtime.Poll(kWorld));
    REQUIRE(runtime.AcceptedSampleCount() == 1);

    transport.AddBatch(
        {Record(1, kBreakpoint, kCoordinateB, kMeshB)},
        kBreakpoint);
    REQUIRE(runtime.Poll(kWorld));
    REQUIRE(runtime.AcceptedSampleCount() == 2);
    REQUIRE(runtime.PublishedCoordinateCount() == 2);

    transport.AddBatch(
        {
            Record(2, kBreakpoint, 0, kMeshA),
            Record(3, kBreakpoint, kCoordinateA, 0),
            Record(4, kBreakpoint + 4, kCoordinateA, kMeshA),
        },
        kBreakpoint,
        3);
    REQUIRE(runtime.Poll(kWorld));
    REQUIRE(runtime.AcceptedSampleCount() == 2);

    InstallSample(
        transport,
        kCoordinateA,
        kMeshA,
        0,
        HardwareBreakpointCoordinate{1.0f, 2.0f, 3.0f});
    transport.AddBatch(
        {Record(5, kBreakpoint, kCoordinateA, kMeshA)},
        kBreakpoint);
    REQUIRE(runtime.Poll(kWorld));
    REQUIRE(runtime.AcceptedSampleCount() == 2);

    InstallSample(
        transport,
        kCoordinateA,
        kMeshA,
        11,
        HardwareBreakpointCoordinate{
            std::numeric_limits<float>::quiet_NaN(),
            2.0f,
            3.0f});
    transport.AddBatch(
        {Record(6, kBreakpoint, kCoordinateA, kMeshA)},
        kBreakpoint);
    REQUIRE(runtime.Poll(kWorld));
    REQUIRE(runtime.AcceptedSampleCount() == 2);

    transport.AddBatch({}, kBreakpoint + 4);
    REQUIRE(!runtime.Poll(kWorld));
    REQUIRE(runtime.PublishedCoordinateCount() == 2);
    REQUIRE(runtime.Stop());
}

}  // namespace

void RunHardwareBreakpointCoordinateRuntimeTests() {
    TestDirectPublicationAndLifecycle();
    TestSampleValidationAndIdentity();
}

#endif
