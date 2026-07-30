#include "game/native/PerfExecutionBreakpoint.h"
#include "game/native/ExecutionBreakpointRecordStore.h"
#include "game/native/MemoryTransport.h"
#include "test_support.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

template <typename Value>
void Append(std::vector<std::uint8_t>& bytes, Value value) {
    const std::size_t offset = bytes.size();
    bytes.resize(offset + sizeof(value));
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void TestArm64RegisterMapping() {
    using namespace
        lengjing::game::native::perf_execution_breakpoint_internal;

    constexpr unsigned int kExtraRegister = 7;
    const std::uint64_t registerMask =
        kArm64RegisterMask | (UINT64_C(1) << kExtraRegister);
    std::vector<std::uint8_t> payload;
    Append(payload, UINT64_C(0x123456789ABCDEF0));
    Append(payload, UINT32_C(321));
    Append(payload, UINT32_C(654));
    Append(payload, UINT64_C(2));
    Append(payload, UINT64_C(0x1000000000000001));
    Append(payload, UINT64_C(0x7000000000000007));
    Append(payload, UINT64_C(0x2000000000000020));
    Append(payload, UINT64_C(0x2100000000000021));
    Append(payload, UINT64_C(0x2300000000000023));
    Append(payload, UINT64_C(0x3100000000000031));
    Append(payload, UINT64_C(0x3200000000000032));

    ParsedSample sample{};
    REQUIRE(ParseArm64SamplePayload(
        payload.data(),
        payload.size(),
        kConfiguredSampleType,
        registerMask,
        sample));
    REQUIRE(sample.processId == 321);
    REQUIRE(sample.threadId == 654);
    REQUIRE(sample.ip == UINT64_C(0x123456789ABCDEF0));
    REQUIRE(sample.x0 == UINT64_C(0x1000000000000001));
    REQUIRE(sample.x20 == UINT64_C(0x2000000000000020));
    REQUIRE(sample.x21 == UINT64_C(0x2100000000000021));
    REQUIRE(sample.x23 == UINT64_C(0x2300000000000023));
    REQUIRE(sample.sp == UINT64_C(0x3100000000000031));
    REQUIRE(sample.pc == UINT64_C(0x3200000000000032));

    sample.ip = UINT64_C(0x4000);
    sample.pc = UINT64_C(0x4000);
    REQUIRE(IsTargetArm64Sample(sample, 321, 654, 0x4000));
    REQUIRE(!IsTargetArm64Sample(sample, 322, 654, 0x4000));
    REQUIRE(!IsTargetArm64Sample(sample, 321, 655, 0x4000));
    REQUIRE(!IsTargetArm64Sample(sample, 321, 654, 0x4004));
    sample.pc = UINT64_C(0x4004);
    REQUIRE(!IsTargetArm64Sample(sample, 321, 654, 0x4000));
}

void TestPayloadValidation() {
    using namespace
        lengjing::game::native::perf_execution_breakpoint_internal;

    std::vector<std::uint8_t> payload;
    Append(payload, UINT64_C(1));
    Append(payload, UINT32_C(100));
    Append(payload, UINT32_C(101));
    Append(payload, UINT64_C(2));
    Append(payload, UINT64_C(10));
    Append(payload, UINT64_C(20));
    Append(payload, UINT64_C(21));
    Append(payload, UINT64_C(23));
    Append(payload, UINT64_C(31));
    Append(payload, UINT64_C(32));

    ParsedSample sample{};
    REQUIRE(ParseArm64SamplePayload(
        payload.data(),
        payload.size(),
        kConfiguredSampleType,
        kArm64RegisterMask,
        sample));
    REQUIRE(!ParseArm64SamplePayload(
        payload.data(),
        payload.size() - 1,
        kConfiguredSampleType,
        kArm64RegisterMask,
        sample));
    REQUIRE(!ParseArm64SamplePayload(
        payload.data(),
        payload.size(),
        kSampleIp | kSampleTid,
        kArm64RegisterMask,
        sample));
    REQUIRE(!ParseArm64SamplePayload(
        payload.data(),
        payload.size(),
        kConfiguredSampleType,
        kArm64RegisterMask &
            ~(UINT64_C(1) << kArm64RegisterX20),
        sample));
    REQUIRE(!ParseArm64SamplePayload(
        payload.data(),
        payload.size(),
        kConfiguredSampleType,
        kArm64RegisterMask &
            ~(UINT64_C(1) << kArm64RegisterX21),
        sample));
    REQUIRE(!ParseArm64SamplePayload(
        payload.data(),
        payload.size(),
        kConfiguredSampleType,
        kArm64RegisterMask &
            ~(UINT64_C(1) << kArm64RegisterX23),
        sample));

    std::uint64_t invalidAbi = 1;
    std::memcpy(
        payload.data() + sizeof(std::uint64_t) +
            2 * sizeof(std::uint32_t),
        &invalidAbi,
        sizeof(invalidAbi));
    REQUIRE(!ParseArm64SamplePayload(
        payload.data(),
        payload.size(),
        kConfiguredSampleType,
        kArm64RegisterMask,
        sample));
}

void TestUnsupportedHostIsExplicit() {
#if !defined(__linux__) || !defined(__aarch64__)
    REQUIRE(!lengjing::game::native::PerfExecutionBreakpoint::
                IsSupported());
#endif
}

lengjing::game::native::ExecutionBreakpointRecord MakeRecord(
    pid_t tid,
    std::uint64_t hitCount,
    std::uintptr_t value) {
    return {
        tid,
        hitCount,
        0x1000,
        value + 1,
        value + 2,
        value + 20,
        value + 21,
        value + 23,
    };
}

void TestPersistentRecordTableRetainsOtherThreads() {
    using lengjing::game::native::ExecutionBreakpointRecord;
    using lengjing::game::native::ExecutionBreakpointRecordStore;
    using lengjing::game::native::kExecutionBreakpointRecordLimit;

    ExecutionBreakpointRecordStore store;
    REQUIRE(store.Store(MakeRecord(10, 1, 0x100)));
    for (std::uint64_t hit = 1; hit <= 1024; ++hit) {
        REQUIRE(store.Store(MakeRecord(20, hit, 0x200 + hit)));
    }

    REQUIRE(store.LatestSize() == 2);
    REQUIRE(store.HistorySize() == kExecutionBreakpointRecordLimit);
    const ExecutionBreakpointRecord* retained = store.FindLatest(10);
    REQUIRE(retained != nullptr);
    REQUIRE(retained->hitCount == 1);
    REQUIRE(retained->x20 == 0x114);
    REQUIRE(retained->x21 == 0x115);
    REQUIRE(retained->x23 == 0x117);

    const ExecutionBreakpointRecord* noisy = store.FindLatest(20);
    REQUIRE(noisy != nullptr);
    REQUIRE(noisy->hitCount == 1024);

    std::array<
        ExecutionBreakpointRecord,
        kExecutionBreakpointRecordLimit> merged{};
    std::size_t totalRecords = 0;
    REQUIRE(store.CopyMerged(
                merged.data(),
                merged.size(),
                totalRecords) == kExecutionBreakpointRecordLimit);
    REQUIRE(totalRecords == kExecutionBreakpointRecordLimit);
    REQUIRE(merged[0].tid == 10);
    REQUIRE(merged[0].hitCount == 1);
    REQUIRE(merged[1].tid == 20);
    REQUIRE(merged[1].hitCount == 1024);
    REQUIRE(merged[2].tid == 20);
    REQUIRE(merged[2].hitCount == 770);
    REQUIRE(merged.back().tid == 20);
    REQUIRE(merged.back().hitCount == 1023);

    std::size_t latestCopies = 0;
    for (const ExecutionBreakpointRecord& record : merged) {
        if (record.tid == 20 && record.hitCount == 1024) {
            ++latestCopies;
        }
    }
    REQUIRE(latestCopies == 1);

    std::array<ExecutionBreakpointRecord, 1> priority{};
    REQUIRE(store.CopyMerged(
                priority.data(),
                priority.size(),
                totalRecords) == 1);
    REQUIRE(totalRecords == kExecutionBreakpointRecordLimit);
    REQUIRE(priority[0].tid == 10);
    REQUIRE(priority[0].hitCount == 1);
}

void TestPersistentRecordTableEvictsOldestThread() {
    using lengjing::game::native::ExecutionBreakpointRecordStore;
    using lengjing::game::native::kExecutionBreakpointRecordLimit;

    ExecutionBreakpointRecordStore store;
    for (std::size_t index = 0;
         index < kExecutionBreakpointRecordLimit;
         ++index) {
        REQUIRE(store.Store(MakeRecord(
            static_cast<pid_t>(index + 1),
            1,
            static_cast<std::uintptr_t>(index))));
    }
    REQUIRE(store.Store(MakeRecord(1, 2, 0x10000)));
    REQUIRE(store.Store(MakeRecord(
        static_cast<pid_t>(kExecutionBreakpointRecordLimit + 1),
        1,
        0x20000)));

    REQUIRE(store.LatestSize() == kExecutionBreakpointRecordLimit);
    REQUIRE(store.FindLatest(1) != nullptr);
    REQUIRE(store.FindLatest(2) == nullptr);
    REQUIRE(store.FindLatest(
                static_cast<pid_t>(
                    kExecutionBreakpointRecordLimit + 1)) != nullptr);
}

void TestPersistentRecordTableCopiesInTidOrder() {
    using lengjing::game::native::ExecutionBreakpointRecord;
    using lengjing::game::native::ExecutionBreakpointRecordStore;

    ExecutionBreakpointRecordStore store;
    REQUIRE(store.Store(MakeRecord(30, 1, 0x300)));
    REQUIRE(store.Store(MakeRecord(10, 1, 0x100)));
    REQUIRE(store.Store(MakeRecord(20, 1, 0x200)));

    std::array<ExecutionBreakpointRecord, 3> records{};
    std::size_t totalRecords = 0;
    REQUIRE(store.CopyMerged(
                records.data(),
                records.size(),
                totalRecords) == 3);
    REQUIRE(totalRecords == 3);
    REQUIRE(records[0].tid == 10);
    REQUIRE(records[1].tid == 20);
    REQUIRE(records[2].tid == 30);

    store.Clear();
    REQUIRE(store.LatestSize() == 0);
    REQUIRE(store.HistorySize() == 0);
    REQUIRE(store.CopyMerged(
                records.data(),
                records.size(),
                totalRecords) == 0);
    REQUIRE(totalRecords == 0);
}

}  // namespace

void RunPerfExecutionBreakpointTests() {
    TestArm64RegisterMapping();
    TestPayloadValidation();
    TestPersistentRecordTableRetainsOtherThreads();
    TestPersistentRecordTableEvictsOldestThread();
    TestPersistentRecordTableCopiesInTidOrder();
    TestUnsupportedHostIsExplicit();
}
