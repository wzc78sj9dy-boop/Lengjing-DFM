#if 0

#include "game/native/PerfExecutionBreakpoint.h"
#include "test_support.h"

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

}  // namespace

void RunPerfExecutionBreakpointTests() {
    TestArm64RegisterMapping();
    TestPayloadValidation();
    TestUnsupportedHostIsExplicit();
}

#endif
