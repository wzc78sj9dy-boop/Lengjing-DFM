#include "game/native/KernelRpcTransport.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

#if defined(__aarch64__) && (defined(__ANDROID__) || defined(__linux__))
#include <unistd.h>
#endif

namespace lengjing::game::native {
namespace {

using kernel_rpc_abi::BatchMemoryItem;
using kernel_rpc_abi::BatchReadPayload;
using kernel_rpc_abi::BatchWritePayload;
using kernel_rpc_abi::ModuleLookupPayload;
using kernel_rpc_abi::Operation;
using kernel_rpc_abi::ProcessLookupPayload;
using kernel_rpc_abi::ProcessValuePayload;
using kernel_rpc_abi::SingleMemoryPayload;

bool IsProcessIdValid(std::int32_t processId) {
    return processId > 0;
}

bool IsProcessHandleValid(kernel_rpc_abi::ProcessHandle processHandle) {
    return processHandle != kernel_rpc_abi::kInvalidProcessHandle;
}

bool IsNameValid(std::string_view name) {
    return !name.empty() &&
        name.size() < 64 &&
        name.find('\0') == std::string_view::npos;
}

bool IsMutableTransferValid(const MutableMemoryTransfer& transfer) {
    return transfer.remoteAddress != 0 &&
        transfer.localBuffer != nullptr &&
        transfer.size != 0;
}

bool IsConstTransferValid(const ConstMemoryTransfer& transfer) {
    return transfer.remoteAddress != 0 &&
        transfer.localBuffer != nullptr &&
        transfer.size != 0;
}

BatchMemoryItem MakeBatchItem(const MutableMemoryTransfer& transfer) {
    return {
        static_cast<std::uint64_t>(transfer.remoteAddress),
        static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(transfer.localBuffer)),
        static_cast<std::uint64_t>(transfer.size),
    };
}

BatchMemoryItem MakeBatchItem(const ConstMemoryTransfer& transfer) {
    return {
        static_cast<std::uint64_t>(transfer.remoteAddress),
        static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(transfer.localBuffer)),
        static_cast<std::uint64_t>(transfer.size),
    };
}

bool IsBatchFallbackError(int result) {
    return result == -ENOTTY || result == -ENOSYS;
}

}  // namespace

KernelRpcTransport::KernelRpcTransport(SyscallInvoker invoker) noexcept
    : invoker_(invoker != nullptr ? invoker : &InvokeSystemCall) {}

long KernelRpcTransport::InvokeSystemCall(long number,
                                          std::uint64_t descriptor,
                                          void* envelope,
                                          std::uint64_t magic) {
#if defined(__aarch64__) && (defined(__ANDROID__) || defined(__linux__))
    return syscall(number, descriptor, envelope, magic);
#else
    (void)number;
    (void)descriptor;
    (void)envelope;
    (void)magic;
    errno = ENOSYS;
    return -1;
#endif
}

int KernelRpcTransport::Invoke(Operation operation, void* payload) const {
    kernel_rpc_abi::Envelope envelope{
        static_cast<std::uint32_t>(operation),
        0,
        payload,
    };
    errno = 0;
    const long result = invoker_(
        kernel_rpc_abi::kSyscallNumber,
        kernel_rpc_abi::kSentinelDescriptor,
        &envelope,
        kernel_rpc_abi::kProtocolMagic);
    if (result == 0) return 0;
    if (result > 0) return -ENOSYS;
    return errno != 0 ? -errno : -EIO;
}

int KernelRpcTransport::ReadMemory(
                                   kernel_rpc_abi::ProcessHandle processHandle,
                                   std::uintptr_t remoteAddress,
                                   void* destination,
                                   std::size_t size) const {
    if (!IsProcessHandleValid(processHandle) || remoteAddress == 0 ||
        destination == nullptr || size == 0) {
        return -EINVAL;
    }
    SingleMemoryPayload payload{
        processHandle,
        0,
        static_cast<std::uint64_t>(remoteAddress),
        static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(destination)),
        static_cast<std::uint64_t>(size),
    };
    return Invoke(Operation::ReadMemory, &payload);
}

int KernelRpcTransport::WriteMemory(
                                    kernel_rpc_abi::ProcessHandle processHandle,
                                    std::uintptr_t remoteAddress,
                                    const void* source,
                                    std::size_t size) const {
    if (!IsProcessHandleValid(processHandle) || remoteAddress == 0 ||
        source == nullptr || size == 0) {
        return -EINVAL;
    }
    SingleMemoryPayload payload{
        processHandle,
        0,
        static_cast<std::uint64_t>(remoteAddress),
        static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(source)),
        static_cast<std::uint64_t>(size),
    };
    return Invoke(Operation::WriteMemory, &payload);
}

int KernelRpcTransport::ReadMemoryBatch(
    kernel_rpc_abi::ProcessHandle processHandle,
    const MutableMemoryTransfer* transfers,
    std::size_t count,
    std::uint8_t* itemStatus) const {
    if (count == 0) {
        return IsProcessHandleValid(processHandle) ? 0 : -EINVAL;
    }
    if (count > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return -EOVERFLOW;
    }
    if (itemStatus != nullptr) {
        std::fill_n(itemStatus, count, static_cast<std::uint8_t>(0));
    }
    if (!IsProcessHandleValid(processHandle) || transfers == nullptr) {
        return -EINVAL;
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (!IsMutableTransferValid(transfers[index])) return -EINVAL;
    }

    int successful = 0;
    try {
        std::vector<BatchMemoryItem> items;
        std::vector<std::uint8_t> status;
        items.reserve(kernel_rpc_abi::kBatchLimit);
        status.resize(kernel_rpc_abi::kBatchLimit);

        for (std::size_t offset = 0; offset < count;) {
            const std::size_t chunkSize = std::min(
                kernel_rpc_abi::kBatchLimit,
                count - offset);
            items.clear();
            for (std::size_t index = 0; index < chunkSize; ++index) {
                items.push_back(MakeBatchItem(transfers[offset + index]));
            }
            std::fill(status.begin(), status.begin() + chunkSize, 0);

            BatchReadPayload payload{
                processHandle,
                static_cast<std::uint32_t>(chunkSize),
                items.data(),
                status.data(),
            };
            const int result = Invoke(Operation::ReadMemoryBatch, &payload);
            if (IsBatchFallbackError(result)) {
                for (std::size_t index = 0; index < chunkSize; ++index) {
                    const auto& transfer = transfers[offset + index];
                    const bool ok = ReadMemory(
                        processHandle,
                        transfer.remoteAddress,
                        transfer.localBuffer,
                        transfer.size) == 0;
                    if (itemStatus != nullptr) {
                        itemStatus[offset + index] = ok ? 1 : 0;
                    }
                    if (ok) ++successful;
                }
            } else if (result < 0) {
                return successful != 0 ? successful : result;
            } else {
                for (std::size_t index = 0; index < chunkSize; ++index) {
                    const bool ok = status[index] != 0;
                    if (itemStatus != nullptr) {
                        itemStatus[offset + index] = ok ? 1 : 0;
                    }
                    if (ok) ++successful;
                }
            }
            offset += chunkSize;
        }
    } catch (const std::bad_alloc&) {
        return -ENOMEM;
    }
    return successful;
}

int KernelRpcTransport::WriteMemoryBatch(
    kernel_rpc_abi::ProcessHandle processHandle,
    const ConstMemoryTransfer* transfers,
    std::size_t count) const {
    if (!IsProcessHandleValid(processHandle)) return -EINVAL;
    if (count == 0) return 0;
    if (transfers == nullptr) return -EINVAL;
    if (count > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return -EOVERFLOW;
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (!IsConstTransferValid(transfers[index])) return -EINVAL;
    }

    int successful = 0;
    try {
        std::vector<BatchMemoryItem> items;
        items.reserve(kernel_rpc_abi::kBatchLimit);

        for (std::size_t offset = 0; offset < count;) {
            const std::size_t chunkSize = std::min(
                kernel_rpc_abi::kBatchLimit,
                count - offset);
            items.clear();
            for (std::size_t index = 0; index < chunkSize; ++index) {
                items.push_back(MakeBatchItem(transfers[offset + index]));
            }

            BatchWritePayload payload{
                processHandle,
                static_cast<std::uint32_t>(chunkSize),
                items.data(),
            };
            const int result = Invoke(Operation::WriteMemoryBatch, &payload);
            if (IsBatchFallbackError(result)) {
                for (std::size_t index = 0; index < chunkSize; ++index) {
                    const auto& transfer = transfers[offset + index];
                    if (WriteMemory(
                            processHandle,
                            transfer.remoteAddress,
                            transfer.localBuffer,
                            transfer.size) == 0) {
                        ++successful;
                    }
                }
            } else if (result < 0) {
                return result;
            } else {
                successful += static_cast<int>(chunkSize);
            }
            offset += chunkSize;
        }
    } catch (const std::bad_alloc&) {
        return -ENOMEM;
    }
    return successful;
}

int KernelRpcTransport::FindProcessId(std::string_view processName,
                                      std::int32_t& processId) const {
    if (!IsNameValid(processName)) return -EINVAL;
    ProcessLookupPayload payload{};
    std::memcpy(payload.processName, processName.data(), processName.size());
    const int result = Invoke(Operation::FindProcessId, &payload);
    if (result == 0) processId = payload.processId;
    return result;
}

int KernelRpcTransport::AttachProcess(
    std::int32_t processId,
    kernel_rpc_abi::ProcessHandle& processHandle) const {
    processHandle = kernel_rpc_abi::kInvalidProcessHandle;
    return IsProcessIdValid(processId) ? -ENOTSUP : -EINVAL;
}

int KernelRpcTransport::FindModuleBase(
                                       kernel_rpc_abi::ProcessHandle processHandle,
                                       std::string_view moduleName,
                                       std::uint64_t& moduleBase) const {
    if (!IsProcessHandleValid(processHandle) || !IsNameValid(moduleName)) {
        return -EINVAL;
    }
    ModuleLookupPayload payload{};
    payload.processHandle = processHandle;
    std::memcpy(payload.moduleName, moduleName.data(), moduleName.size());
    const int result = Invoke(Operation::FindModuleBase, &payload);
    if (result == 0) moduleBase = payload.moduleBase;
    return result;
}

int KernelRpcTransport::ReadTpidrEl0(std::int32_t processId,
                                     std::uint64_t& value) const {
    if (!IsProcessIdValid(processId)) return -EINVAL;
    ProcessValuePayload payload{};
    payload.processId = static_cast<std::uint32_t>(processId);
    const int result = Invoke(Operation::ReadTpidrEl0, &payload);
    if (result == 0) value = payload.value;
    return result;
}

int KernelRpcTransport::ReadProcessPacKeys(
    std::int32_t processId,
    kernel_rpc_abi::ProcessPacKeysPayload& keys) const {
    if (!IsProcessIdValid(processId)) return -EINVAL;
    kernel_rpc_abi::ProcessPacKeysPayload payload{};
    payload.processId = processId;
    const int result = Invoke(Operation::ReadProcessPacKeys, &payload);
    if (result == 0) keys = payload;
    return result;
}

int KernelRpcTransport::InstallApgaKey(
    const kernel_rpc_abi::PacKey128& key) const {
    kernel_rpc_abi::PacKey128 payload = key;
    return Invoke(Operation::InstallApgaKey, &payload);
}

int KernelRpcTransport::ReleaseProcessHandle(
    kernel_rpc_abi::ProcessHandle processHandle) const {
    if (!IsProcessHandleValid(processHandle)) return -EINVAL;
    kernel_rpc_abi::ProcessHandle payload = processHandle;
    return Invoke(Operation::ReleaseProcessHandle, &payload);
}

int KernelRpcTransport::InvokeControlWord(std::uint64_t& value) const {
    return Invoke(Operation::ControlWord, &value);
}

}  // namespace lengjing::game::native
