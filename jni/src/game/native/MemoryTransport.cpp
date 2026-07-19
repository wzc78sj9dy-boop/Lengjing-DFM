#include "game/native/MemoryTransport.h"

#include "game/native/KernelModuleLoader.h"
#include "game/native/KernelRpcTransport.h"
#include "paradise/paradise_api.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>

namespace lengjing::game::native {
namespace {

constexpr std::uintptr_t kMinimumRemoteAddress = 0x10000000ULL;
constexpr std::uintptr_t kMaximumRemoteAddress = 0x10000000000ULL;

bool IsNumericName(const char* name) {
    if (name == nullptr || *name == '\0') return false;
    for (const char* cursor = name; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') return false;
    }
    return true;
}

std::string ReadFirstToken(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::string value;
    std::getline(input, value, '\0');
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

bool IsRemoteRangeValid(std::uintptr_t address, std::size_t size) {
    return address >= kMinimumRemoteAddress &&
        address < kMaximumRemoteAddress &&
        size > 0 &&
        size <= kMaximumRemoteAddress - address;
}

bool ProcessVmTransferExact(
    pid_t processId,
    std::uintptr_t address,
    void* localBuffer,
    std::size_t size,
    bool write) {
    auto* bytes = static_cast<std::uint8_t*>(localBuffer);
    std::size_t completed = 0;
    while (completed < size) {
        iovec local{
            bytes + completed,
            size - completed,
        };
        iovec remote{
            reinterpret_cast<void*>(address + completed),
            size - completed,
        };
        const long callNumber = write
            ? __NR_process_vm_writev
            : __NR_process_vm_readv;
        const ssize_t result = static_cast<ssize_t>(syscall(
            callNumber,
            processId,
            &local,
            1UL,
            &remote,
            1UL,
            0UL));
        if (result > 0) {
            completed += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

std::string MutableName(std::string_view value) {
    return std::string(value.begin(), value.end());
}

std::string RpcFailureText(int result) {
    if (result >= 0) return std::to_string(result);
    const int errorNumber = -result;
    const char* message = std::strerror(errorNumber);
    return message != nullptr
        ? std::string(message)
        : std::to_string(result);
}

}  // namespace

struct MemoryTransport::Impl {
    MemoryTransportMode mode = MemoryTransportMode::ProcessVm;
    pid_t processId = -1;
    paradise_driver* kernel = nullptr;
    std::unique_ptr<KernelRpcTransport> privateRpc;
    kernel_rpc_abi::ProcessHandle privateProcessHandle =
        kernel_rpc_abi::kInvalidProcessHandle;
    bool open = false;
    bool writable = false;
    std::uint64_t ioGeneration = 0;
    mutable std::mutex ioMutex;

    ~Impl() {
        Close();
    }

    void ResetUnlocked() noexcept {
        ++ioGeneration;
        if (privateRpc != nullptr &&
            privateProcessHandle != kernel_rpc_abi::kInvalidProcessHandle) {
            static_cast<void>(privateRpc->ReleaseProcessHandle(
                privateProcessHandle));
        }
        privateRpc.reset();
        privateProcessHandle = kernel_rpc_abi::kInvalidProcessHandle;
        delete kernel;
        kernel = nullptr;
        mode = MemoryTransportMode::ProcessVm;
        processId = -1;
        open = false;
        writable = false;
    }

    void Close() noexcept {
        std::lock_guard<std::mutex> lock(ioMutex);
        ResetUnlocked();
    }

    bool Open(int modeIndex,
              pid_t targetProcessId,
              std::string_view processName,
              std::string& error) {
        std::lock_guard<std::mutex> lock(ioMutex);
        ResetUnlocked();
        if (!IsValidMemoryTransportMode(modeIndex) || targetProcessId <= 0) {
            error = "内存读取模式参数无效";
            return false;
        }

        mode = static_cast<MemoryTransportMode>(modeIndex);
        processId = targetProcessId;
        switch (mode) {
            case MemoryTransportMode::ProcessVm:
                writable = true;
                break;
            case MemoryTransportMode::KernelDriver:
                if (!EnsureKernelDriverReady(error)) {
                    ResetUnlocked();
                    return false;
                }
                kernel = new (std::nothrow) paradise_driver();
                if (kernel == nullptr) {
                    error = "无法创建内核读取接口";
                    ResetUnlocked();
                    return false;
                }
                kernel->initialize(processId);
                writable = true;
                break;
            case MemoryTransportMode::PrivateRpc: {
                privateRpc.reset(new (std::nothrow) KernelRpcTransport());
                if (privateRpc == nullptr) {
                    error = "无法创建内核 RPC 接口";
                    ResetUnlocked();
                    return false;
                }
                std::int32_t resolvedProcessId = -1;
                const int result = privateRpc->FindProcessId(
                    processName, resolvedProcessId);
                if (result != 0) {
                    error = "内核 RPC 接口不可用: " +
                        RpcFailureText(result);
                    ResetUnlocked();
                    return false;
                }
                if (resolvedProcessId != processId) {
                    error = "内核 RPC 返回的进程标识不匹配";
                    ResetUnlocked();
                    return false;
                }
                const int attachResult = privateRpc->AttachProcess(
                    resolvedProcessId, privateProcessHandle);
                if (attachResult != 0) {
                    error = attachResult == -ENOTSUP
                        ? "内核 RPC 进程附加协议尚未解析"
                        : "内核 RPC 无法附加目标进程: " +
                            RpcFailureText(attachResult);
                    ResetUnlocked();
                    return false;
                }
                writable = true;
                break;
            }
            case MemoryTransportMode::Count:
            default:
                error = "内存读取模式参数无效";
                ResetUnlocked();
                return false;
        }

        open = true;
        error.clear();
        return true;
    }

    bool ReadUnlocked(std::uintptr_t address,
                      void* destination,
                      std::size_t size) {
        if (!open || destination == nullptr ||
            !IsRemoteRangeValid(address, size)) {
            return false;
        }

        if (mode == MemoryTransportMode::ProcessVm) {
            return ProcessVmTransferExact(
                processId, address, destination, size, false);
        }
        if (mode == MemoryTransportMode::KernelDriver) {
            return kernel != nullptr &&
                kernel->read_fast(address, destination, size);
        }
        return mode == MemoryTransportMode::PrivateRpc &&
            privateRpc != nullptr &&
            privateProcessHandle != kernel_rpc_abi::kInvalidProcessHandle &&
             privateRpc->ReadMemory(
                 privateProcessHandle, address, destination, size) == 0;
    }

    bool Read(std::uintptr_t address, void* destination, std::size_t size) {
        std::lock_guard<std::mutex> lock(ioMutex);
        return ReadUnlocked(address, destination, size);
    }

    bool ReadAtGeneration(std::uint64_t expectedGeneration,
                          std::uintptr_t address,
                          void* destination,
                          std::size_t size,
                          bool& generationMatches) {
        std::lock_guard<std::mutex> lock(ioMutex);
        generationMatches = ioGeneration == expectedGeneration;
        return generationMatches && ReadUnlocked(address, destination, size);
    }

    std::size_t ReadBatch(const MemoryReadRequest* requests,
                          std::size_t count,
                          std::uint8_t* itemStatus) {
        std::unique_lock<std::mutex> lock(ioMutex);
        if (count == 0) return 0;
        if (itemStatus != nullptr) {
            std::fill_n(itemStatus, count, static_cast<std::uint8_t>(0));
        }
        if (!open || requests == nullptr) return 0;

        for (std::size_t index = 0; index < count; ++index) {
            if (requests[index].localBuffer == nullptr ||
                !IsRemoteRangeValid(
                    requests[index].remoteAddress, requests[index].size)) {
                return 0;
            }
        }

        const std::uint64_t batchGeneration = ioGeneration;
        if (mode == MemoryTransportMode::PrivateRpc &&
            privateRpc != nullptr &&
            privateProcessHandle != kernel_rpc_abi::kInvalidProcessHandle) {
            try {
                std::vector<MutableMemoryTransfer> transfers;
                transfers.reserve(count);
                for (std::size_t index = 0; index < count; ++index) {
                    transfers.push_back(MutableMemoryTransfer{
                        requests[index].remoteAddress,
                        requests[index].localBuffer,
                        requests[index].size,
                    });
                }
                const int result = privateRpc->ReadMemoryBatch(
                    privateProcessHandle,
                    transfers.data(),
                    transfers.size(),
                    itemStatus);
                return result > 0 ? static_cast<std::size_t>(result) : 0;
            } catch (...) {
                return 0;
            }
        }

        lock.unlock();
        std::size_t successful = 0;
        for (std::size_t index = 0; index < count; ++index) {
            bool generationMatches = false;
            const bool read = ReadAtGeneration(
                batchGeneration,
                requests[index].remoteAddress,
                requests[index].localBuffer,
                requests[index].size,
                generationMatches);
            if (!generationMatches) break;
            if (itemStatus != nullptr) itemStatus[index] = read ? 1 : 0;
            if (read) ++successful;
        }
        return successful;
    }

    bool Write(std::uintptr_t address, const void* source, std::size_t size) {
        std::lock_guard<std::mutex> lock(ioMutex);
        if (!open || !writable || source == nullptr ||
            !IsRemoteRangeValid(address, size)) {
            return false;
        }

        if (mode == MemoryTransportMode::ProcessVm) {
            return ProcessVmTransferExact(
                processId,
                address,
                const_cast<void*>(source),
                size,
                true);
        }
        if (mode == MemoryTransportMode::KernelDriver) {
            return kernel != nullptr &&
                kernel->write_fast(
                    address,
                    const_cast<void*>(source),
                    size);
        }
        return mode == MemoryTransportMode::PrivateRpc &&
            privateRpc != nullptr &&
            privateProcessHandle != kernel_rpc_abi::kInvalidProcessHandle &&
            privateRpc->WriteMemory(
                privateProcessHandle, address, source, size) == 0;
    }

    std::uintptr_t ModuleBase(std::string_view moduleName) {
        std::lock_guard<std::mutex> lock(ioMutex);
        if (!open) return 0;
        const std::string name = MutableName(moduleName);
        if (mode == MemoryTransportMode::KernelDriver) {
            return kernel != nullptr
                ? kernel->get_module_base(name.c_str())
                : 0;
        }
        if (mode == MemoryTransportMode::PrivateRpc &&
            privateRpc != nullptr &&
            privateProcessHandle != kernel_rpc_abi::kInvalidProcessHandle) {
            std::uint64_t moduleBase = 0;
            return privateRpc->FindModuleBase(
                       privateProcessHandle, moduleName, moduleBase) == 0
                ? static_cast<std::uintptr_t>(moduleBase)
                : 0;
        }
        return 0;
    }

    bool IsOpen() const noexcept {
        std::lock_guard<std::mutex> lock(ioMutex);
        return open;
    }

    bool CanWrite() const noexcept {
        std::lock_guard<std::mutex> lock(ioMutex);
        return open && writable;
    }

    bool UsesKernelBackend() const noexcept {
        std::lock_guard<std::mutex> lock(ioMutex);
        if (!open || !IsKernelMemoryTransportMode(mode)) return false;
        return mode == MemoryTransportMode::KernelDriver
            ? kernel != nullptr
            : privateRpc != nullptr &&
                privateProcessHandle != kernel_rpc_abi::kInvalidProcessHandle;
    }

    bool ReadProcessExecutionContext(
        ProcessExecutionContext& context) const {
        std::lock_guard<std::mutex> lock(ioMutex);
        context = {};
        if (!open || processId <= 0) return false;

        KernelRpcTransport rpc;
        std::uint64_t tpidrEl0 = 0;
        kernel_rpc_abi::ProcessPacKeysPayload keys{};
        if (rpc.ReadTpidrEl0(processId, tpidrEl0) != 0 ||
            rpc.ReadProcessPacKeys(processId, keys) != 0) {
            return false;
        }
        const ProcessExecutionContext candidate{
            tpidrEl0,
            keys.apga.low,
            keys.apga.high,
        };
        if (!candidate.IsUsable()) return false;
        context = candidate;
        return true;
    }
};

MemoryTransport::MemoryTransport() : impl_(std::make_unique<Impl>()) {}

MemoryTransport::~MemoryTransport() = default;

bool MemoryTransport::Open(int modeIndex,
                           pid_t processId,
                           std::string_view processName,
                           std::string& error) {
    return impl_ != nullptr &&
        impl_->Open(modeIndex, processId, processName, error);
}

void MemoryTransport::Close() noexcept {
    if (impl_ != nullptr) impl_->Close();
}

bool MemoryTransport::Read(
    std::uintptr_t address,
    void* destination,
    std::size_t size) {
    return impl_ != nullptr && impl_->Read(address, destination, size);
}

std::size_t MemoryTransport::ReadBatch(
    const MemoryReadRequest* requests,
    std::size_t count,
    std::uint8_t* itemStatus) {
    return impl_ != nullptr
        ? impl_->ReadBatch(requests, count, itemStatus)
        : 0;
}

bool MemoryTransport::Write(
    std::uintptr_t address,
    const void* source,
    std::size_t size) {
    return impl_ != nullptr && impl_->Write(address, source, size);
}

std::uintptr_t MemoryTransport::ModuleBase(std::string_view moduleName) {
    return impl_ != nullptr ? impl_->ModuleBase(moduleName) : 0;
}

bool MemoryTransport::IsOpen() const noexcept {
    return impl_ != nullptr && impl_->IsOpen();
}

bool MemoryTransport::CanWrite() const noexcept {
    return impl_ != nullptr && impl_->CanWrite();
}

bool MemoryTransport::UsesKernelBackend() const noexcept {
    return impl_ != nullptr && impl_->UsesKernelBackend();
}

bool MemoryTransport::ReadProcessExecutionContext(
    ProcessExecutionContext& context) const {
    context = {};
    return impl_ != nullptr && impl_->ReadProcessExecutionContext(context);
}

pid_t FindProcessId(std::string_view processName) {
    if (processName.empty()) return -1;
    DIR* directory = opendir("/proc");
    if (directory == nullptr) return -1;

    pid_t result = -1;
    while (dirent* entry = readdir(directory)) {
        if (!IsNumericName(entry->d_name)) continue;
        const pid_t candidate =
            static_cast<pid_t>(std::strtol(entry->d_name, nullptr, 10));
        if (candidate <= 0) continue;
        const std::string prefix = std::string("/proc/") + entry->d_name;
        std::string value = ReadFirstToken(prefix + "/cmdline");
        if (value.empty()) value = ReadFirstToken(prefix + "/comm");
        if (value == processName) {
            result = candidate;
            break;
        }
    }
    closedir(directory);
    return result;
}

bool IsProcessAlive(pid_t processId) {
    if (processId <= 0) return false;
    if (kill(processId, 0) == 0 || errno == EPERM) return true;
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%d", processId);
    struct stat info {};
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

std::uintptr_t FindMappedModuleBase(
    pid_t processId,
    std::string_view moduleName) {
    if (processId <= 0 || moduleName.empty()) return 0;
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%d/maps", processId);
    std::ifstream input(path);
    if (!input) return 0;

    std::string line;
    while (std::getline(input, line)) {
        if (line.find(moduleName) == std::string::npos) continue;
        const std::size_t separator = line.find('-');
        if (separator == std::string::npos) continue;
        const std::string start = line.substr(0, separator);
        char* end = nullptr;
        const unsigned long long value =
            std::strtoull(start.c_str(), &end, 16);
        if (end != start.c_str() &&
            value >= kMinimumRemoteAddress &&
            value < kMaximumRemoteAddress) {
            return static_cast<std::uintptr_t>(value);
        }
    }
    return 0;
}

}  // namespace lengjing::game::native
