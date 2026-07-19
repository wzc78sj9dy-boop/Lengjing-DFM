#include "game/native/MemoryTransport.h"

#include "game/native/KernelModuleLoader.h"
#include "game/native/KernelRpcTransport.h"
#include "game/native/PtraceExecutionContextProvider.h"
#include "game/native/ThreadContextDeviceTransport.h"
#include "game/native/ThreadExecutionContextProvider.h"
#include "paradise/paradise_api.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <fcntl.h>
#include <limits>
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
constexpr char kDefaultThreadContextDevice[] = "/dev/fbe775";
constexpr char kDefaultThreadContextName[] = "GameThread";
constexpr std::uintptr_t kCoordinateRootRva = 0x0E738950ULL;
constexpr std::uintptr_t kCoordinateBridgeFieldOffset = 12;
constexpr std::uintptr_t kCoordinateEntryFieldOffset = 0xA0;
constexpr std::uint64_t kPointerPayloadMask =
    UINT64_C(0x0000FFFFFFFFFFFF);
constexpr std::uint32_t kPacgaX8X8X9 = UINT32_C(0x9AC93108);
constexpr std::uint64_t kCoordinatePacgaData = UINT64_C(0x412625C7);
constexpr std::uint64_t kCoordinatePacgaModifier = UINT64_C(0xBB7AC00B);
constexpr std::size_t kMaximumOracleMappingSize = 2U * 1024U * 1024U;
constexpr std::size_t kOracleScanChunkSize = 4096;

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

std::size_t ThreadContextRequestCount() {
    const char* value = std::getenv("LENGJING_THREAD_CONTEXT_REQUEST_COUNT");
    if (value != nullptr && std::string_view(value) == "0x400") {
        return thread_context_device_abi::kSmallRequestCount;
    }
    return thread_context_device_abi::kLargeRequestCount;
}

thread_context_device_abi::WriteSuccessPolicy
ThreadContextWriteSuccessPolicy() {
    using thread_context_device_abi::WriteSuccessPolicy;
    const char* value = std::getenv("LENGJING_THREAD_CONTEXT_SUCCESS");
    if (value == nullptr || std::string_view(value) == "zero") {
        return WriteSuccessPolicy::ExactZero;
    }
    if (std::string_view(value) == "count") {
        return WriteSuccessPolicy::ExactRequestCount;
    }
    if (std::string_view(value) == "zero-or-count") {
        return WriteSuccessPolicy::ZeroOrRequestCount;
    }
    if (std::string_view(value) == "nonnegative") {
        return WriteSuccessPolicy::AnyNonNegative;
    }
    return WriteSuccessPolicy::ExactZero;
}

bool FindExecutableMapping(pid_t processId,
                           std::uintptr_t address,
                           std::uintptr_t& start,
                           std::uintptr_t& end) {
    start = 0;
    end = 0;
    if (processId <= 0 || address == 0) return false;

    std::ifstream maps(
        "/proc/" + std::to_string(processId) + "/maps",
        std::ios::binary);
    std::string line;
    while (std::getline(maps, line)) {
        unsigned long long candidateStart = 0;
        unsigned long long candidateEnd = 0;
        char permissions[5]{};
        if (std::sscanf(
                line.c_str(),
                "%llx-%llx %4s",
                &candidateStart,
                &candidateEnd,
                permissions) != 3) {
            continue;
        }
        if (address < candidateStart || address >= candidateEnd ||
            permissions[2] != 'x' || candidateEnd <= candidateStart ||
            candidateEnd - candidateStart > kMaximumOracleMappingSize) {
            continue;
        }
        start = static_cast<std::uintptr_t>(candidateStart);
        end = static_cast<std::uintptr_t>(candidateEnd);
        return true;
    }
    return false;
}

}  // namespace

struct MemoryTransport::Impl {
    enum class ExecutionContextSource : std::uint8_t {
        None,
        Device,
        PtraceOracle,
    };

    MemoryTransportMode mode = MemoryTransportMode::ProcessVm;
    pid_t processId = -1;
    paradise_driver* kernel = nullptr;
    std::unique_ptr<KernelRpcTransport> privateRpc;
    kernel_rpc_abi::ProcessHandle privateProcessHandle =
        kernel_rpc_abi::kInvalidProcessHandle;
    int threadContextFd = -1;
    std::unique_ptr<ThreadContextDeviceTransport> threadContextTransport;
    std::unique_ptr<ThreadExecutionContextProvider> threadContextProvider;
    PtracePacgaOracleReader ptraceContextReader;
    std::unique_ptr<PtraceExecutionContextProvider> ptraceContextProvider;
    PacgaOracleInstruction ptraceOracleInstruction{};
    ExecutionContextSource executionContextSource =
        ExecutionContextSource::None;
    std::chrono::steady_clock::time_point nextThreadContextOpen{};
    std::chrono::steady_clock::time_point nextPtraceOracleResolve{};
    bool open = false;
    bool writable = false;
    std::uint64_t ioGeneration = 0;
    mutable std::mutex ioMutex;

    ~Impl() {
        Close();
    }

    void ResetUnlocked() noexcept {
        ++ioGeneration;
        CloseThreadContextUnlocked();
        ptraceContextProvider.reset();
        ptraceOracleInstruction = {};
        executionContextSource = ExecutionContextSource::None;
        nextThreadContextOpen = {};
        nextPtraceOracleResolve = {};
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

    void CloseThreadContextUnlocked() noexcept {
        threadContextProvider.reset();
        threadContextTransport.reset();
        if (threadContextFd >= 0) {
            ::close(threadContextFd);
            threadContextFd = -1;
        }
    }

    bool OpenThreadContextUnlocked() {
        if (threadContextProvider != nullptr) return true;
        const auto now = std::chrono::steady_clock::now();
        if (nextThreadContextOpen.time_since_epoch().count() != 0 &&
            now < nextThreadContextOpen) {
            return false;
        }
        nextThreadContextOpen = now + std::chrono::seconds(1);
        const char* configuredDevice =
            std::getenv("LENGJING_THREAD_CONTEXT_DEVICE");
        const char* device = configuredDevice != nullptr &&
                configuredDevice[0] != '\0'
            ? configuredDevice
            : kDefaultThreadContextDevice;
        threadContextFd = ::open(device, O_RDWR | O_CLOEXEC);
        if (threadContextFd < 0) return false;

        const thread_context_device_abi::Profile profile{
            threadContextFd,
            ThreadContextRequestCount(),
            ThreadContextWriteSuccessPolicy(),
        };
        threadContextTransport =
            std::make_unique<ThreadContextDeviceTransport>(profile);
        const char* configuredName =
            std::getenv("LENGJING_COORDINATE_THREAD_NAME");
        const char* threadName = configuredName != nullptr &&
                configuredName[0] != '\0'
            ? configuredName
            : kDefaultThreadContextName;
        threadContextProvider =
            std::make_unique<ThreadExecutionContextProvider>(
                processId,
                *threadContextTransport,
                threadName);
        return true;
    }

    bool ResolvePtraceOracleInstructionUnlocked(
        PacgaOracleInstruction& instruction) {
        instruction = {};
        const auto now = std::chrono::steady_clock::now();
        if (nextPtraceOracleResolve.time_since_epoch().count() != 0 &&
            now < nextPtraceOracleResolve) {
            if (ptraceOracleInstruction.IsValid()) {
                instruction = ptraceOracleInstruction;
                return true;
            }
            return false;
        }
        nextPtraceOracleResolve = now + std::chrono::seconds(1);

        std::uintptr_t moduleBase = 0;
        if (mode == MemoryTransportMode::KernelDriver && kernel != nullptr) {
            moduleBase = kernel->get_module_base("libUE4.so");
        } else if (mode == MemoryTransportMode::PrivateRpc &&
                   privateRpc != nullptr &&
                   privateProcessHandle !=
                       kernel_rpc_abi::kInvalidProcessHandle) {
            std::uint64_t resolved = 0;
            if (privateRpc->FindModuleBase(
                    privateProcessHandle, "libUE4.so", resolved) == 0) {
                moduleBase = static_cast<std::uintptr_t>(resolved);
            }
        }
        if (!IsRemoteRangeValid(moduleBase, 4)) {
            moduleBase = FindMappedModuleBase(processId, "libUE4.so");
        }
        if (!IsRemoteRangeValid(moduleBase, 4) ||
            moduleBase >
                std::numeric_limits<std::uintptr_t>::max() -
                    kCoordinateRootRva - kCoordinateBridgeFieldOffset) {
            ptraceOracleInstruction = {};
            return false;
        }

        std::uint64_t rawBridge = 0;
        if (!ReadUnlocked(
                moduleBase + kCoordinateRootRva +
                    kCoordinateBridgeFieldOffset,
                &rawBridge,
                sizeof(rawBridge))) {
            ptraceOracleInstruction = {};
            return false;
        }
        const std::uintptr_t bridge = static_cast<std::uintptr_t>(
            rawBridge & kPointerPayloadMask);
        if (bridge > std::numeric_limits<std::uintptr_t>::max() -
                kCoordinateEntryFieldOffset ||
            !IsRemoteRangeValid(
                bridge + kCoordinateEntryFieldOffset,
                sizeof(std::uint64_t))) {
            ptraceOracleInstruction = {};
            return false;
        }

        std::uint64_t rawEntry = 0;
        if (!ReadUnlocked(
                bridge + kCoordinateEntryFieldOffset,
                &rawEntry,
                sizeof(rawEntry))) {
            ptraceOracleInstruction = {};
            return false;
        }
        const std::uintptr_t entry = static_cast<std::uintptr_t>(
            rawEntry & kPointerPayloadMask);
        std::uintptr_t mappingStart = 0;
        std::uintptr_t mappingEnd = 0;
        if (!FindExecutableMapping(
                processId, entry, mappingStart, mappingEnd)) {
            ptraceOracleInstruction = {};
            return false;
        }

        const auto scan = [this](std::uintptr_t begin,
                                 std::uintptr_t end,
                                 std::uintptr_t& found) {
            found = 0;
            begin = (begin + 3U) & ~std::uintptr_t{3U};
            if (end <= begin) return false;
            std::array<std::uint8_t, kOracleScanChunkSize> bytes{};
            for (std::uintptr_t cursor = begin; cursor < end;) {
                const std::size_t remaining =
                    static_cast<std::size_t>(end - cursor);
                const std::size_t size = std::min(bytes.size(), remaining);
                if (!ReadUnlocked(cursor, bytes.data(), size)) return false;
                for (std::size_t offset = 0;
                     offset + sizeof(std::uint32_t) <= size;
                     offset += sizeof(std::uint32_t)) {
                    std::uint32_t encoded = 0;
                    std::memcpy(
                        &encoded, bytes.data() + offset, sizeof(encoded));
                    if (encoded == kPacgaX8X8X9) {
                        found = cursor + offset;
                        return true;
                    }
                }
                cursor += size;
            }
            return false;
        };

        std::uintptr_t found = 0;
        const std::uintptr_t preferredEnd = std::min(
            mappingEnd,
            entry <= std::numeric_limits<std::uintptr_t>::max() - 0x5000U
                ? entry + 0x5000U
                : mappingEnd);
        if (!scan(entry, preferredEnd, found) &&
            !scan(mappingStart, mappingEnd, found)) {
            ptraceOracleInstruction = {};
            return false;
        }
        ptraceOracleInstruction = {
            found,
            kCoordinatePacgaData,
            kCoordinatePacgaModifier,
        };
        instruction = ptraceOracleInstruction;
        return true;
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

        static_cast<void>(OpenThreadContextUnlocked());
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

    bool ReadProcessExecutionContext(ProcessExecutionContext& context) {
        std::lock_guard<std::mutex> lock(ioMutex);
        context = {};
        executionContextSource = ExecutionContextSource::None;
        if (!open || processId <= 0) return false;

        if (OpenThreadContextUnlocked() &&
            threadContextProvider != nullptr) {
            ThreadExecutionContextRefresh refresh =
                threadContextProvider->Refresh();
            if (!refresh.HasContext() &&
                (refresh.status == -EBADF || refresh.status == -ENODEV ||
                 refresh.status == -ENXIO)) {
                CloseThreadContextUnlocked();
                nextThreadContextOpen = {};
                if (OpenThreadContextUnlocked() &&
                    threadContextProvider != nullptr) {
                    refresh = threadContextProvider->Refresh();
                }
            }
            if (refresh.HasContext()) {
                const ThreadExecutionContextSnapshot& snapshot =
                    refresh.snapshot;
                const ProcessExecutionContext candidate{
                    snapshot.tpidrEl0,
                    snapshot.apga.low,
                    snapshot.apga.high,
                    snapshot.threadId,
                    snapshot.threadStartTimeTicks,
                    snapshot.generation,
                };
                if (candidate.IsUsable()) {
                    context = candidate;
                    executionContextSource =
                        ExecutionContextSource::Device;
                    return true;
                }
            }
        }

        PacgaOracleInstruction instruction{};
        if (!ResolvePtraceOracleInstructionUnlocked(instruction)) {
            return false;
        }
        if (ptraceContextProvider == nullptr) {
            const char* configuredName =
                std::getenv("LENGJING_COORDINATE_THREAD_NAME");
            const char* threadName = configuredName != nullptr &&
                    configuredName[0] != '\0'
                ? configuredName
                : kDefaultThreadContextName;
            ptraceContextProvider =
                std::make_unique<PtraceExecutionContextProvider>(
                    processId,
                    ptraceContextReader,
                    threadName);
        }
        const PtraceExecutionContextRefresh refresh =
            ptraceContextProvider->Refresh(instruction);
        if (!refresh.HasContext()) return false;
        const PtraceExecutionContextSnapshot& snapshot = refresh.snapshot;
        ProcessExecutionContext candidate{};
        candidate.tpidrEl0 = snapshot.tpidrEl0;
        candidate.threadId = snapshot.threadId;
        candidate.threadStartTimeTicks = snapshot.threadStartTimeTicks;
        candidate.generation = snapshot.generation;
        candidate.pacgaOracle = {
            snapshot.instruction.data,
            snapshot.instruction.modifier,
            snapshot.result,
            true,
        };
        if (!candidate.IsUsable()) return false;
        context = candidate;
        executionContextSource = ExecutionContextSource::PtraceOracle;
        return true;
    }

    bool RejectProcessExecutionContext() noexcept {
        std::lock_guard<std::mutex> lock(ioMutex);
        if (!open) return false;
        if (executionContextSource == ExecutionContextSource::Device &&
            threadContextProvider != nullptr) {
            executionContextSource = ExecutionContextSource::None;
            return threadContextProvider->RejectCurrent();
        }
        if (executionContextSource ==
                ExecutionContextSource::PtraceOracle &&
            ptraceContextProvider != nullptr) {
            executionContextSource = ExecutionContextSource::None;
            return ptraceContextProvider->RejectCurrent();
        }
        return false;
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
    ProcessExecutionContext& context) {
    context = {};
    return impl_ != nullptr && impl_->ReadProcessExecutionContext(context);
}

bool MemoryTransport::RejectProcessExecutionContext() noexcept {
    return impl_ != nullptr && impl_->RejectProcessExecutionContext();
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
