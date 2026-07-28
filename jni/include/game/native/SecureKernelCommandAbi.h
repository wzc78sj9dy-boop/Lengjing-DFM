#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace lengjing::game::native::secure_kernel_abi {

inline constexpr std::uint32_t kGetModuleCommand = 0xC1184402U;
inline constexpr std::uint32_t kReadMemoryCommand = 0xC0204403U;
inline constexpr std::uint32_t kGetThreadTlsCommand = 0xC0104408U;
inline constexpr std::uint32_t kGetThreadTidCommand = 0xC1084409U;
inline constexpr std::uint32_t kExecuteComputationCommand = 0xC020440CU;
inline constexpr std::uint32_t kGetExecutionCapabilitiesCommand =
    0xC010440EU;
inline constexpr std::uint32_t kInstallExecutionBreakpointsCommand =
    0x4110440FU;
inline constexpr std::uint32_t kPollExecutionBreakpointsCommand =
    0xC0184410U;
inline constexpr std::uint32_t kRemoveExecutionBreakpointsCommand =
    0x00004411U;
inline constexpr std::uint32_t kGetThreadStackCommand = 0xC0204412U;

inline constexpr std::uint64_t kHandshakeA = 0x44464D50ULL;
inline constexpr std::uint64_t kHandshakeB = 0x504D4644ULL;

struct alignas(8) ModuleRequest final {
    std::int32_t processId = -1;
    char moduleName[256]{};
    std::uint32_t reserved = 0;
    std::uint64_t base = 0;
    std::uint64_t size = 0;
};

struct alignas(8) MemoryRequest final {
    std::int32_t processId = -1;
    std::uint32_t reserved = 0;
    std::uint64_t remoteAddress = 0;
    std::uint64_t size = 0;
    std::uint64_t localAddress = 0;
};

struct alignas(8) ThreadTlsRequest final {
    std::int32_t threadId = -1;
    std::uint32_t reserved = 0;
    std::uint64_t tls = 0;
};

struct alignas(8) ThreadTidRequest final {
    std::int32_t processId = -1;
    char threadName[256]{};
    std::int32_t threadId = -1;
};

struct alignas(8) ComputationRequest final {
    std::int32_t processId = -1;
    std::uint32_t supported = 0;
    std::uint64_t data = 0;
    std::uint64_t modifier = 0;
    std::uint64_t result = 0;
};

struct alignas(8) ThreadStackRequest final {
    std::int32_t threadId = -1;
    std::uint32_t found = 0;
    std::uint64_t low = 0;
    std::uint64_t high = 0;
    std::uint64_t tls = 0;
};

struct alignas(8) ExecutionCapabilities final {
    std::uint32_t breakpointCount = 0;
    std::uint32_t reserved04 = 0;
    std::uint32_t eventCount = 0;
    std::uint32_t supported = 0;
};

struct alignas(8) ExecutionBreakpointSpec final {
    std::uint32_t type = 0;
    std::uint32_t length = 0;
    std::uint64_t address = 0;
};

struct alignas(8) InstallExecutionBreakpointsRequest final {
    std::uint32_t threadId = 0;
    std::uint32_t count = 0;
    std::uint64_t reserved08 = 0;
    std::array<ExecutionBreakpointSpec, 16> breakpoints{};
};

struct alignas(8) ExecutionEvent final {
    std::uint32_t slotTag = 0;
    std::uint32_t kind = 0;
    std::uint64_t counter = 0;
    std::uint64_t pc0 = 0;
    std::uint64_t pc1 = 0;
    std::uint64_t state20 = 0;
    std::uint64_t pc2 = 0;
    std::array<std::uint8_t, 0x18> state30{};
    std::array<std::uint64_t, 31> x{};
    std::uint32_t fpsr = 0;
    std::uint32_t fpcr = 0;
    std::array<std::array<std::uint8_t, 16>, 32> vectors{};
};

struct alignas(8) ExecutionEventSlot final {
    std::uint32_t type = 0;
    std::uint32_t length = 0;
    std::uint32_t count = 0;
    std::uint32_t reserved0C = 0;
    std::uint64_t address = 0;
    std::uint64_t aggregate = 0;
    std::array<ExecutionEvent, 64> events{};
};

struct alignas(8) ExecutionPollBuffer final {
    std::uint32_t threadId = 0;
    std::uint32_t flags = 0;
    std::uint32_t slotCount = 0;
    std::array<std::uint8_t, 0x0C> reserved0C{};
    std::array<ExecutionEventSlot, 16> slots{};
};

struct alignas(8) PollExecutionBreakpointsRequest final {
    std::uint64_t buffer = 0;
    std::uint64_t bytes = 0;
    std::uint64_t reserved = 0;
};

constexpr bool IsPageAligned(std::uint64_t address) noexcept {
    return (address & 0xFFFULL) == 0;
}

constexpr bool IsRemoteUserAddress(std::uint64_t address) noexcept {
    return address >= 0x4000000001ULL &&
        address <= 0xFFFFFFFFFFULL;
}

constexpr std::uint64_t NormalizeRemoteAddress(
    std::uint64_t address) noexcept {
    const std::uint64_t untagged = address & 0x00FFFFFFFFFFFFFFULL;
    if (IsRemoteUserAddress(untagged)) return untagged;
    if (address >= 0xFFFFFF0000000000ULL) {
        const std::uint64_t compact = address & 0xFFFFFFFFFFULL;
        if (IsRemoteUserAddress(compact)) return compact;
    }
    return address;
}

constexpr bool IsValidStackRange(
    const ThreadStackRequest& request,
    std::uint64_t expectedTls) noexcept {
    const std::uint64_t low = NormalizeRemoteAddress(request.low);
    const std::uint64_t high = NormalizeRemoteAddress(request.high);
    const std::uint64_t tls = NormalizeRemoteAddress(request.tls);
    return request.found != 0 &&
        low != 0 &&
        high > low &&
        IsPageAligned(low) &&
        IsPageAligned(high) &&
        tls == NormalizeRemoteAddress(expectedTls);
}

static_assert(sizeof(ModuleRequest) == 0x118);
static_assert(offsetof(ModuleRequest, moduleName) == 0x04);
static_assert(offsetof(ModuleRequest, reserved) == 0x104);
static_assert(offsetof(ModuleRequest, base) == 0x108);
static_assert(offsetof(ModuleRequest, size) == 0x110);

static_assert(sizeof(MemoryRequest) == 0x20);
static_assert(offsetof(MemoryRequest, remoteAddress) == 0x08);
static_assert(offsetof(MemoryRequest, size) == 0x10);
static_assert(offsetof(MemoryRequest, localAddress) == 0x18);

static_assert(sizeof(ThreadTlsRequest) == 0x10);
static_assert(offsetof(ThreadTlsRequest, tls) == 0x08);

static_assert(sizeof(ThreadTidRequest) == 0x108);
static_assert(offsetof(ThreadTidRequest, threadName) == 0x04);
static_assert(offsetof(ThreadTidRequest, threadId) == 0x104);

static_assert(sizeof(ComputationRequest) == 0x20);
static_assert(offsetof(ComputationRequest, supported) == 0x04);
static_assert(offsetof(ComputationRequest, data) == 0x08);
static_assert(offsetof(ComputationRequest, modifier) == 0x10);
static_assert(offsetof(ComputationRequest, result) == 0x18);

static_assert(sizeof(ThreadStackRequest) == 0x20);
static_assert(offsetof(ThreadStackRequest, found) == 0x04);
static_assert(offsetof(ThreadStackRequest, low) == 0x08);
static_assert(offsetof(ThreadStackRequest, high) == 0x10);
static_assert(offsetof(ThreadStackRequest, tls) == 0x18);

static_assert(sizeof(ExecutionCapabilities) == 0x10);
static_assert(sizeof(ExecutionBreakpointSpec) == 0x10);
static_assert(sizeof(InstallExecutionBreakpointsRequest) == 0x110);
static_assert(
    offsetof(InstallExecutionBreakpointsRequest, reserved08) == 0x08);
static_assert(
    offsetof(InstallExecutionBreakpointsRequest, breakpoints) == 0x10);
static_assert(sizeof(ExecutionEvent) == 0x348);
static_assert(offsetof(ExecutionEvent, x) == 0x48);
static_assert(offsetof(ExecutionEvent, fpsr) == 0x140);
static_assert(offsetof(ExecutionEvent, fpcr) == 0x144);
static_assert(offsetof(ExecutionEvent, vectors) == 0x148);
static_assert(sizeof(ExecutionEventSlot) == 0xD220);
static_assert(offsetof(ExecutionEventSlot, events) == 0x20);
static_assert(sizeof(ExecutionPollBuffer) == 0xD2218);
static_assert(offsetof(ExecutionPollBuffer, slots) == 0x18);
static_assert(sizeof(PollExecutionBreakpointsRequest) == 0x18);

}  // namespace lengjing::game::native::secure_kernel_abi
