#include "game/native/SecureKernelAssetPolicy.h"
#include "game/native/SecureKernelCommandAbi.h"

#include <cstddef>
#include <cstdint>

namespace {

using namespace lengjing::game::native::secure_kernel_abi;
using lengjing::game::native::SecureKernelAsset;
using lengjing::game::native::SelectSecureKernelAsset;

constexpr bool TestCommandValues() {
    return kGetModuleCommand == 0xC1184402U &&
        kReadMemoryCommand == 0xC0204403U &&
        kGetThreadTlsCommand == 0xC0104408U &&
        kGetThreadTidCommand == 0xC1084409U &&
        kExecuteComputationCommand == 0xC020440CU &&
        kGetExecutionCapabilitiesCommand == 0xC010440EU &&
        kInstallExecutionBreakpointsCommand == 0x4110440FU &&
        kPollExecutionBreakpointsCommand == 0xC0184410U &&
        kRemoveExecutionBreakpointsCommand == 0x00004411U &&
        kGetThreadStackCommand == 0xC0204412U;
}

constexpr bool TestPointerNormalization() {
    return NormalizeRemoteAddress(0xAB00007000100000ULL) ==
            0x7000100000ULL &&
        NormalizeRemoteAddress(0xFFFFFF7000200000ULL) ==
            0x7000200000ULL &&
        NormalizeRemoteAddress(0x1234ULL) == 0x1234ULL;
}

constexpr bool TestAssetSelection() {
    return SelectSecureKernelAsset("5.10.252-dirty") ==
            SecureKernelAsset::Kernel510 &&
        SelectSecureKernelAsset("5.15.202-android13") ==
            SecureKernelAsset::Kernel515 &&
        SelectSecureKernelAsset("6.1.166-dirty") ==
            SecureKernelAsset::Kernel61 &&
        SelectSecureKernelAsset("6.6.127-custom") ==
            SecureKernelAsset::Kernel66 &&
        SelectSecureKernelAsset("6.12.76-custom") ==
            SecureKernelAsset::Kernel612 &&
        SelectSecureKernelAsset("6.2.0") ==
            SecureKernelAsset::None;
}

constexpr bool TestStackValidation() {
    ThreadStackRequest valid{};
    valid.found = 1;
    valid.low = 0x7000000000ULL;
    valid.high = 0x7000008000ULL;
    valid.tls = 0x7000002000ULL;
    if (!IsValidStackRange(valid, 0xAB00007000002000ULL)) {
        return false;
    }
    valid.low += 1;
    if (IsValidStackRange(valid, 0x7000002000ULL)) return false;
    valid.low -= 1;
    valid.tls += 8;
    return !IsValidStackRange(valid, 0x7000002000ULL);
}

static_assert(TestCommandValues());
static_assert(TestPointerNormalization());
static_assert(TestAssetSelection());
static_assert(TestStackValidation());
static_assert(sizeof(ModuleRequest) == 0x118);
static_assert(sizeof(MemoryRequest) == 0x20);
static_assert(sizeof(ThreadTlsRequest) == 0x10);
static_assert(sizeof(ThreadTidRequest) == 0x108);
static_assert(sizeof(ComputationRequest) == 0x20);
static_assert(sizeof(ThreadStackRequest) == 0x20);
static_assert(sizeof(ExecutionCapabilities) == 0x10);
static_assert(sizeof(InstallExecutionBreakpointsRequest) == 0x110);
static_assert(
    offsetof(InstallExecutionBreakpointsRequest, breakpoints) == 0x10);
static_assert(sizeof(ExecutionEvent) == 0x348);
static_assert(sizeof(ExecutionEventSlot) == 0xD220);
static_assert(sizeof(ExecutionPollBuffer) == 0xD2218);
static_assert(sizeof(PollExecutionBreakpointsRequest) == 0x18);

}  // namespace

int main() {
    return TestCommandValues() &&
            TestPointerNormalization() &&
            TestAssetSelection() &&
            TestStackValidation()
        ? 0
        : 1;
}
