#include "game/native/PerfExecutionBreakpoint.h"
#include "game/native/ExecutionBreakpointRecordHistory.h"
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
}

void TestChronologicalRecordHistory() {
    using lengjing::game::native::ExecutionBreakpointRecord;
    using lengjing::game::native::ExecutionBreakpointRecordHistory;
    using lengjing::game::native::kExecutionBreakpointRecordLimit;

    ExecutionBreakpointRecordHistory history;
    history.Push({1, 1, 0x1000, 0, 0, 0, 0x21, 0x100});
    history.Push({1, 2, 0x1000, 0, 0, 0, 0x21, 0x200});

    std::array<ExecutionBreakpointRecord, 2> duplicate{};
    REQUIRE(history.CopyNewest(
                duplicate.data(), duplicate.size()) == 2);
    REQUIRE(duplicate[0].hitCount == 1);
    REQUIRE(duplicate[1].hitCount == 2);
    REQUIRE(duplicate[0].x21 == duplicate[1].x21);

    for (std::size_t index = 0;
         index < kExecutionBreakpointRecordLimit;
         ++index) {
        history.Push({
            2,
            static_cast<std::uint64_t>(index + 3),
            0x2000,
            0,
            0,
            0,
            static_cast<std::uintptr_t>(index),
            static_cast<std::uintptr_t>(index + 0x300),
        });
    }
    REQUIRE(history.Size() == kExecutionBreakpointRecordLimit);

    std::array<ExecutionBreakpointRecord, 3> newest{};
    REQUIRE(history.CopyNewest(newest.data(), newest.size()) == 3);
    REQUIRE(newest[0].hitCount ==
            kExecutionBreakpointRecordLimit);
    REQUIRE(newest[1].hitCount ==
            kExecutionBreakpointRecordLimit + 1);
    REQUIRE(newest[2].hitCount ==
            kExecutionBreakpointRecordLimit + 2);

    history.Clear();
    REQUIRE(history.Size() == 0);
    REQUIRE(history.CopyNewest(newest.data(), newest.size()) == 0);
}

}  // namespace

void RunPerfExecutionBreakpointTests() {
    TestArm64RegisterMapping();
    TestPayloadValidation();
    TestChronologicalRecordHistory();
    TestUnsupportedHostIsExplicit();
}

#endif
