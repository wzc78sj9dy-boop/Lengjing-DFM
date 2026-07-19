#include "game/native/KernelRpcTransport.h"
#include "game/native/MemoryTransport.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using lengjing::game::native::ConstMemoryTransfer;
using lengjing::game::native::KernelRpcTransport;
using lengjing::game::native::MutableMemoryTransfer;
namespace abi = lengjing::game::native::kernel_rpc_abi;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            throw std::runtime_error(                                           \
                std::string(__FILE__) + ":" + std::to_string(__LINE__) +      \
                ": check failed: " #condition);                               \
        }                                                                       \
    } while (false)

std::function<long(abi::Operation, void*)> gHandler;

long FakeSyscall(long number,
                 std::uint64_t descriptor,
                 void* envelopeAddress,
                 std::uint64_t magic) {
    CHECK(number == abi::kSyscallNumber);
    CHECK(descriptor == UINT64_C(0x00000000FFFFFFFF));
    CHECK(descriptor == abi::kSentinelDescriptor);
    CHECK(magic == abi::kProtocolMagic);
    CHECK(envelopeAddress != nullptr);
    auto* envelope = static_cast<abi::Envelope*>(envelopeAddress);
    CHECK(envelope->reserved == 0);
    CHECK(gHandler);
    return gHandler(
        static_cast<abi::Operation>(envelope->operation),
        envelope->payload);
}

void TestErrorNormalization() {
    KernelRpcTransport transport(&FakeSyscall);
    gHandler = [](abi::Operation operation, void*) {
        CHECK(operation == abi::Operation::ReleaseProcessHandle);
        errno = EACCES;
        return -1L;
    };
    CHECK(transport.ReleaseProcessHandle(42) == -EACCES);

    gHandler = [](abi::Operation, void*) {
        errno = 0;
        return -1L;
    };
    CHECK(transport.ReleaseProcessHandle(42) == -EIO);

    gHandler = [](abi::Operation, void*) {
        errno = 0;
        return 7L;
    };
    CHECK(transport.ReleaseProcessHandle(42) == -ENOSYS);
    CHECK(transport.ReleaseProcessHandle(0) == -EINVAL);
}

void TestMemoryTransportModePolicy() {
    using lengjing::game::native::IsKernelMemoryTransportMode;
    using lengjing::game::native::IsValidMemoryTransportMode;
    using lengjing::game::native::MemoryTransportMode;
    using lengjing::game::native::kMemoryTransportModeCount;

    CHECK(kMemoryTransportModeCount == 3);
    CHECK(IsValidMemoryTransportMode(0));
    CHECK(IsValidMemoryTransportMode(1));
    CHECK(IsValidMemoryTransportMode(2));
    CHECK(!IsValidMemoryTransportMode(-1));
    CHECK(!IsValidMemoryTransportMode(3));
    CHECK(!IsKernelMemoryTransportMode(MemoryTransportMode::ProcessVm));
    CHECK(IsKernelMemoryTransportMode(MemoryTransportMode::KernelDriver));
    CHECK(IsKernelMemoryTransportMode(MemoryTransportMode::PrivateRpc));
}

void TestSingleMemoryTransfers() {
    KernelRpcTransport transport(&FakeSyscall);
    std::uint32_t destination = 0;
    const std::uint32_t expected = 0x12345678U;
    gHandler = [&](abi::Operation operation, void* payloadAddress) {
        CHECK(operation == abi::Operation::ReadMemory);
        auto* payload = static_cast<abi::SingleMemoryPayload*>(payloadAddress);
        CHECK(payload->processHandle == 77U);
        CHECK(payload->reserved == 0);
        CHECK(payload->remoteAddress == 0x1000U);
        CHECK(payload->size == sizeof(destination));
        std::memcpy(
            reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(payload->localAddress)),
            &expected,
            sizeof(expected));
        return 0L;
    };
    CHECK(transport.ReadMemory(
              77, 0x1000U, &destination, sizeof(destination)) == 0);
    CHECK(destination == expected);

    const std::uint32_t source = 0xAABBCCDDU;
    gHandler = [&](abi::Operation operation, void* payloadAddress) {
        CHECK(operation == abi::Operation::WriteMemory);
        auto* payload = static_cast<abi::SingleMemoryPayload*>(payloadAddress);
        CHECK(payload->processHandle == 77U);
        CHECK(payload->remoteAddress == 0x2000U);
        CHECK(payload->size == sizeof(source));
        CHECK(*reinterpret_cast<const std::uint32_t*>(
                  static_cast<std::uintptr_t>(payload->localAddress)) == source);
        return 0L;
    };
    CHECK(transport.WriteMemory(77, 0x2000U, &source, sizeof(source)) == 0);
    CHECK(transport.ReadMemory(77, 0, &destination, sizeof(destination)) ==
          -EINVAL);
}

void TestMetadataOperations() {
    KernelRpcTransport transport(&FakeSyscall);

    gHandler = [](abi::Operation operation, void* payloadAddress) {
        CHECK(operation == abi::Operation::FindProcessId);
        auto* payload = static_cast<abi::ProcessLookupPayload*>(payloadAddress);
        CHECK(std::string(payload->processName) == "com.example.game");
        payload->processId = 321;
        return 0L;
    };
    std::int32_t processId = 0;
    CHECK(transport.FindProcessId("com.example.game", processId) == 0);
    CHECK(processId == 321);

    abi::ProcessHandle processHandle = 99;
    CHECK(transport.AttachProcess(321, processHandle) == -ENOTSUP);
    CHECK(processHandle == abi::kInvalidProcessHandle);
    CHECK(transport.AttachProcess(0, processHandle) == -EINVAL);

    gHandler = [](abi::Operation operation, void* payloadAddress) {
        CHECK(operation == abi::Operation::FindModuleBase);
        auto* payload = static_cast<abi::ModuleLookupPayload*>(payloadAddress);
        CHECK(payload->processHandle == 321U);
        CHECK(std::string(payload->moduleName) == "libGame.so");
        payload->moduleBase = UINT64_C(0x7000000000);
        return 0L;
    };
    std::uint64_t moduleBase = 0;
    CHECK(transport.FindModuleBase(321, "libGame.so", moduleBase) == 0);
    CHECK(moduleBase == UINT64_C(0x7000000000));

    gHandler = [](abi::Operation operation, void* payloadAddress) {
        CHECK(operation == abi::Operation::ReadTpidrEl0);
        auto* payload = static_cast<abi::ProcessValuePayload*>(payloadAddress);
        CHECK(payload->processId == 321U);
        payload->value = UINT64_C(0x7F00112233);
        return 0L;
    };
    std::uint64_t tpidr = 0;
    CHECK(transport.ReadTpidrEl0(321, tpidr) == 0);
    CHECK(tpidr == UINT64_C(0x7F00112233));

    gHandler = [](abi::Operation operation, void* payloadAddress) {
        CHECK(operation == abi::Operation::ReadProcessPacKeys);
        auto* payload =
            static_cast<abi::ProcessPacKeysPayload*>(payloadAddress);
        CHECK(payload->processId == 321);
        payload->apga = {11, 22};
        return 0L;
    };
    abi::ProcessPacKeysPayload keys{};
    CHECK(transport.ReadProcessPacKeys(321, keys) == 0);
    CHECK(keys.apga.low == 11);
    CHECK(keys.apga.high == 22);

    gHandler = [](abi::Operation operation, void* payloadAddress) {
        CHECK(operation == abi::Operation::InstallApgaKey);
        const auto* payload = static_cast<const abi::PacKey128*>(payloadAddress);
        CHECK(payload->low == 11);
        CHECK(payload->high == 22);
        return 0L;
    };
    CHECK(transport.InstallApgaKey(keys.apga) == 0);
}

void TestBatchReadAndChunking() {
    KernelRpcTransport transport(&FakeSyscall);
    std::vector<std::uint64_t> output(1025);
    std::vector<MutableMemoryTransfer> transfers;
    transfers.reserve(output.size());
    for (std::size_t index = 0; index < output.size(); ++index) {
        transfers.push_back({0x100000U + index * 8, &output[index], 8});
    }
    std::vector<std::uint8_t> itemStatus(output.size(), 9);
    int batchCalls = 0;
    gHandler = [&](abi::Operation operation, void* payloadAddress) {
        CHECK(operation == abi::Operation::ReadMemoryBatch);
        auto* payload = static_cast<abi::BatchReadPayload*>(payloadAddress);
        CHECK(payload->processHandle == 99U);
        CHECK(payload->count <= abi::kBatchLimit);
        ++batchCalls;
        for (std::uint32_t index = 0; index < payload->count; ++index) {
            *reinterpret_cast<std::uint64_t*>(
                static_cast<std::uintptr_t>(
                    payload->items[index].localAddress)) =
                payload->items[index].remoteAddress;
            payload->status[index] = index == 1 && batchCalls == 1 ? 0 : 1;
        }
        return 0L;
    };
    CHECK(transport.ReadMemoryBatch(
              99, transfers.data(), transfers.size(), itemStatus.data()) ==
          1024);
    CHECK(batchCalls == 2);
    CHECK(itemStatus[0] == 1);
    CHECK(itemStatus[1] == 0);
    CHECK(itemStatus[1024] == 1);
    CHECK(output[1024] == transfers[1024].remoteAddress);
}

void TestBatchFallbacks() {
    KernelRpcTransport transport(&FakeSyscall);
    std::uint32_t outputs[3]{};
    MutableMemoryTransfer reads[] = {
        {0x1000U, &outputs[0], sizeof(outputs[0])},
        {0x2000U, &outputs[1], sizeof(outputs[1])},
        {0x3000U, &outputs[2], sizeof(outputs[2])},
    };
    std::uint8_t readStatus[3]{};
    int singleReads = 0;
    gHandler = [&](abi::Operation operation, void* payloadAddress) {
        if (operation == abi::Operation::ReadMemoryBatch) {
            errno = ENOTTY;
            return -1L;
        }
        CHECK(operation == abi::Operation::ReadMemory);
        auto* payload = static_cast<abi::SingleMemoryPayload*>(payloadAddress);
        ++singleReads;
        if (payload->remoteAddress == 0x2000U) {
            errno = EFAULT;
            return -1L;
        }
        *reinterpret_cast<std::uint32_t*>(
            static_cast<std::uintptr_t>(payload->localAddress)) =
            static_cast<std::uint32_t>(payload->remoteAddress);
        return 0L;
    };
    CHECK(transport.ReadMemoryBatch(55, reads, 3, readStatus) == 2);
    CHECK(singleReads == 3);
    CHECK(readStatus[0] == 1);
    CHECK(readStatus[1] == 0);
    CHECK(readStatus[2] == 1);

    const std::uint32_t sources[] = {1, 2};
    ConstMemoryTransfer writes[] = {
        {0x4000U, &sources[0], sizeof(sources[0])},
        {0x5000U, &sources[1], sizeof(sources[1])},
    };
    int singleWrites = 0;
    gHandler = [&](abi::Operation operation, void*) {
        if (operation == abi::Operation::WriteMemoryBatch) {
            errno = ENOSYS;
            return -1L;
        }
        CHECK(operation == abi::Operation::WriteMemory);
        ++singleWrites;
        return 0L;
    };
    CHECK(transport.WriteMemoryBatch(55, writes, 2) == 2);
    CHECK(singleWrites == 2);
}

void TestBatchStatusResetOnInvalidInput() {
    KernelRpcTransport transport(&FakeSyscall);
    std::uint32_t outputs[3]{};
    MutableMemoryTransfer reads[] = {
        {0x1000U, &outputs[0], sizeof(outputs[0])},
        {0, &outputs[1], sizeof(outputs[1])},
        {0x3000U, &outputs[2], sizeof(outputs[2])},
    };
    std::uint8_t status[] = {9, 9, 9};
    CHECK(transport.ReadMemoryBatch(55, reads, 3, status) == -EINVAL);
    CHECK(status[0] == 0);
    CHECK(status[1] == 0);
    CHECK(status[2] == 0);

    status[0] = 9;
    status[1] = 9;
    status[2] = 9;
    CHECK(transport.ReadMemoryBatch(0, reads, 3, status) == -EINVAL);
    CHECK(status[0] == 0);
    CHECK(status[1] == 0);
    CHECK(status[2] == 0);
}

void TestBatchReadPartialSuccess() {
    KernelRpcTransport transport(&FakeSyscall);
    std::vector<std::uint64_t> output(1025);
    std::vector<MutableMemoryTransfer> transfers;
    transfers.reserve(output.size());
    for (std::size_t index = 0; index < output.size(); ++index) {
        transfers.push_back({0x200000U + index * 8, &output[index], 8});
    }
    std::vector<std::uint8_t> status(output.size(), 9);
    int batchCalls = 0;
    gHandler = [&](abi::Operation operation, void* payloadAddress) {
        CHECK(operation == abi::Operation::ReadMemoryBatch);
        auto* payload = static_cast<abi::BatchReadPayload*>(payloadAddress);
        ++batchCalls;
        if (batchCalls == 2) {
            errno = EIO;
            return -1L;
        }
        CHECK(payload->count == abi::kBatchLimit);
        for (std::uint32_t index = 0; index < payload->count; ++index) {
            payload->status[index] = 1;
        }
        return 0L;
    };
    CHECK(transport.ReadMemoryBatch(
              55, transfers.data(), transfers.size(), status.data()) ==
          static_cast<int>(abi::kBatchLimit));
    CHECK(batchCalls == 2);
    CHECK(status[0] == 1);
    CHECK(status[1023] == 1);
    CHECK(status[1024] == 0);
}

}  // namespace

int main() {
    try {
        TestMemoryTransportModePolicy();
        TestErrorNormalization();
        TestSingleMemoryTransfers();
        TestMetadataOperations();
        TestBatchReadAndChunking();
        TestBatchFallbacks();
        TestBatchStatusResetOnInvalidInput();
        TestBatchReadPartialSuccess();
        std::cout << "kernel RPC transport tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
