#pragma once

#if 0

#include <cstddef>
#include <cstdint>
#include <memory>

#include <sys/types.h>

namespace lengjing::game::native {

struct ExecutionBreakpointRecord;

namespace perf_execution_breakpoint_internal {

inline constexpr std::uint64_t kSampleIp = UINT64_C(1) << 0;
inline constexpr std::uint64_t kSampleTid = UINT64_C(1) << 1;
inline constexpr std::uint64_t kSampleRegsUser = UINT64_C(1) << 12;
inline constexpr std::uint64_t kConfiguredSampleType =
    kSampleIp | kSampleTid | kSampleRegsUser;

inline constexpr unsigned int kArm64RegisterX0 = 0;
inline constexpr unsigned int kArm64RegisterX23 = 23;
inline constexpr unsigned int kArm64RegisterSp = 31;
inline constexpr unsigned int kArm64RegisterPc = 32;
inline constexpr std::uint64_t kArm64RegisterMask =
    (UINT64_C(1) << kArm64RegisterX0) |
    (UINT64_C(1) << kArm64RegisterX23) |
    (UINT64_C(1) << kArm64RegisterSp) |
    (UINT64_C(1) << kArm64RegisterPc);

struct ParsedSample {
    std::int32_t processId = -1;
    std::int32_t threadId = -1;
    std::uint64_t ip = 0;
    std::uint64_t pc = 0;
    std::uint64_t sp = 0;
    std::uint64_t x0 = 0;
    std::uint64_t x23 = 0;
};

bool ParseArm64SamplePayload(
    const std::uint8_t* payload,
    std::size_t payloadSize,
    std::uint64_t sampleType,
    std::uint64_t registerMask,
    ParsedSample& sample) noexcept;

bool IsTargetArm64Sample(const ParsedSample& sample,
                         std::int32_t processId,
                         std::int32_t threadId,
                         std::uintptr_t address) noexcept;

}  // namespace perf_execution_breakpoint_internal

class PerfExecutionBreakpoint final {
public:
    PerfExecutionBreakpoint();
    ~PerfExecutionBreakpoint();

    PerfExecutionBreakpoint(const PerfExecutionBreakpoint&) = delete;
    PerfExecutionBreakpoint& operator=(const PerfExecutionBreakpoint&) =
        delete;

    static bool IsSupported() noexcept;

    bool Configure(pid_t processId, std::uintptr_t address) noexcept;
    bool ReadRecords(ExecutionBreakpointRecord* records,
                     std::size_t capacity,
                     std::size_t& recordsRead,
                     std::uintptr_t& hitAddress,
                     std::size_t& totalRecords) noexcept;
    bool Remove() noexcept;
    bool IsConfigured() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lengjing::game::native

#endif
