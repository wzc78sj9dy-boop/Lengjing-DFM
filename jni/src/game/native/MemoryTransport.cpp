#include "game/native/MemoryTransport.h"

#include "game/native/KernelModuleLoader.h"
#include "game/native/KernelRpcTransport.h"
#if 0
#include "game/native/PerfExecutionBreakpoint.h"
#endif
#include "game/native/PacgaOperandResolver.h"
#include "game/native/PtraceExecutionContextProvider.h"
#include "game/native/ThreadContextDeviceTransport.h"
#include "game/native/ThreadExecutionContextProvider.h"
#include "platform/PerformanceTrace.h"
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

#if 0
static_assert(sizeof(hwbp_point_config) == 24,
              "unexpected Paradise breakpoint point layout");
static_assert(sizeof(hwbp_record) == 848,
              "unexpected Paradise breakpoint record layout");
static_assert(HWBP_MAX_RECORDS == kExecutionBreakpointRecordLimit,
              "Paradise breakpoint record limit mismatch");
#endif

constexpr std::uintptr_t kMinimumRemoteAddress = 0x10000000ULL;
constexpr std::uintptr_t kMaximumRemoteAddress = 0x10000000000ULL;
constexpr char kDefaultThreadContextDevice[] = "/dev/fbe775";
constexpr char kDefaultThreadContextName[] = "GameThread";
constexpr std::uint64_t kPointerPayloadMask =
    UINT64_C(0x0000FFFFFFFFFFFF);
constexpr std::uint32_t kPacgaX8X8X9 = UINT32_C(0x9AC93108);
constexpr std::size_t kMaximumOracleMappingSize = 2U * 1024U * 1024U;
constexpr std::size_t kOracleScanChunkSize = 4096;
constexpr std::size_t kOracleImmediateWindowInstructions = 16;
constexpr std::size_t kOracleEntryFingerprintSize = 64;
constexpr std::size_t kOracleMaximumFingerprintWindowSize =
    (kOracleImmediateWindowInstructions + 1U) * sizeof(std::uint32_t);
constexpr int kOracleStableSnapshotAttempts = 3;
constexpr std::uint64_t kOracleFingerprintOffset =
    UINT64_C(14695981039346656037);
constexpr std::uint64_t kOracleFingerprintPrime = UINT64_C(1099511628211);

std::uint64_t HashOracleBytes(const void* data,
                              std::size_t size,
                              std::uint64_t hash =
                                  kOracleFingerprintOffset) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= kOracleFingerprintPrime;
    }
    return hash;
}

template <typename Value>
std::uint64_t HashOracleValue(std::uint64_t hash,
                              const Value& value) noexcept {
    return HashOracleBytes(&value, sizeof(value), hash);
}

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

struct MemoryTransferResult {
    int status = 0;
    std::size_t completed = 0;
};

struct MemoryBatchTransferResult {
    int status = 0;
    std::size_t completed = 0;
    std::size_t failedIndex = 0;
    std::size_t failedCompleted = 0;
};

MemoryTransferResult ProcessVmTransferExact(
    pid_t processId,
    std::uintptr_t address,
    void* localBuffer,
    std::size_t size,
    bool write) {
    auto* bytes = static_cast<std::uint8_t*>(localBuffer);
    MemoryTransferResult transfer{};
    while (transfer.completed < size) {
        iovec local{
            bytes + transfer.completed,
            size - transfer.completed,
        };
        iovec remote{
            reinterpret_cast<void*>(address + transfer.completed),
            size - transfer.completed,
        };
#if LENGJING_ENABLE_PROJECTILE_TRACKING
        const long callNumber = write
            ? __NR_process_vm_writev
            : __NR_process_vm_readv;
#else
        static_cast<void>(write);
        constexpr long callNumber = __NR_process_vm_readv;
#endif
        errno = 0;
        const ssize_t result = static_cast<ssize_t>(syscall(
            callNumber,
            processId,
            &local,
            1UL,
            &remote,
            1UL,
            0UL));
        if (result > 0) {
            transfer.completed += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        transfer.status = result == 0
            ? -ENODATA
            : (errno != 0 ? -errno : -EIO);
        return transfer;
    }
    return transfer;
}

MemoryTransferResult ProcessMemReadExact(
    int descriptor,
    std::uintptr_t address,
    void* localBuffer,
    std::size_t size) {
    auto* bytes = static_cast<std::uint8_t*>(localBuffer);
    MemoryTransferResult transfer{};
    while (transfer.completed < size) {
        errno = 0;
        const ssize_t result = ::pread(
            descriptor,
            bytes + transfer.completed,
            size - transfer.completed,
            static_cast<off_t>(address + transfer.completed));
        if (result > 0) {
            transfer.completed += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) continue;
        transfer.status = result == 0
            ? -ENODATA
            : (errno != 0 ? -errno : -EIO);
        return transfer;
    }
    return transfer;
}

MemoryBatchTransferResult ProcessVmReadBatchOnce(
    pid_t processId,
    const MemoryReadRequest* requests,
    std::size_t count,
    std::size_t totalSize) {
    MemoryBatchTransferResult transfer{};
    std::array<iovec, kCoordinateMemoryBatchRequestLimit> local{};
    std::array<iovec, kCoordinateMemoryBatchRequestLimit> remote{};
    for (std::size_t index = 0; index < count; ++index) {
        local[index] = iovec{
            requests[index].localBuffer,
            requests[index].size,
        };
        remote[index] = iovec{
            reinterpret_cast<void*>(requests[index].remoteAddress),
            requests[index].size,
        };
    }

    ssize_t result = 0;
    do {
        errno = 0;
        result = static_cast<ssize_t>(syscall(
            __NR_process_vm_readv,
            processId,
            local.data(),
            static_cast<unsigned long>(count),
            remote.data(),
            static_cast<unsigned long>(count),
            0UL));
    } while (result < 0 && errno == EINTR);

    if (result == static_cast<ssize_t>(totalSize)) {
        transfer.completed = totalSize;
        transfer.failedIndex = count;
        return transfer;
    }
    if (result > 0 && static_cast<std::size_t>(result) <= totalSize) {
        transfer.completed = static_cast<std::size_t>(result);
        transfer.status = -ENODATA;
    } else {
        transfer.status = result == 0
            ? -ENODATA
            : (errno != 0 ? -errno : -EIO);
    }

    std::size_t remaining = transfer.completed;
    for (std::size_t index = 0; index < count; ++index) {
        if (remaining < requests[index].size) {
            transfer.failedIndex = index;
            transfer.failedCompleted = remaining;
            return transfer;
        }
        remaining -= requests[index].size;
    }
    transfer.failedIndex = count - 1;
    transfer.failedCompleted = requests[count - 1].size;
    return transfer;
}

MemoryBatchTransferResult ProcessMemReadBatchExact(
    int descriptor,
    const MemoryReadRequest* requests,
    std::size_t count) {
    MemoryBatchTransferResult transfer{};
    for (std::size_t index = 0; index < count; ++index) {
        const MemoryTransferResult item = ProcessMemReadExact(
            descriptor,
            requests[index].remoteAddress,
            requests[index].localBuffer,
            requests[index].size);
        transfer.completed += item.completed;
        if (item.status != 0 || item.completed != requests[index].size) {
            transfer.status = item.status != 0 ? item.status : -ENODATA;
            transfer.failedIndex = index;
            transfer.failedCompleted = item.completed;
            return transfer;
        }
    }
    transfer.failedIndex = count;
    return transfer;
}

std::size_t CoordinateBatchRequestedBytes(
    const MemoryReadRequest* requests,
    std::size_t count) noexcept {
    if (requests == nullptr || count == 0 ||
        count > kCoordinateMemoryBatchRequestLimit) {
        return 0;
    }
    std::size_t total = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (requests[index].size >
            std::numeric_limits<std::size_t>::max() - total) {
            return 0;
        }
        total += requests[index].size;
    }
    return total;
}

bool IsStructuralCoordinateBatchFailure(int status) noexcept {
    return status == -EINVAL || status == -ENOSYS || status == -E2BIG ||
        status == -EOPNOTSUPP;
}

CoordinateReadFailure ClassifyCoordinateReadFailure(
    CoordinateReadFailure fallback,
    const MemoryTransferResult& transfer) noexcept {
    if (transfer.status == -EACCES || transfer.status == -EPERM) {
        return CoordinateReadFailure::PermissionDenied;
    }
    if (transfer.status == -EFAULT) {
        return CoordinateReadFailure::AddressFault;
    }
    if (transfer.status == -ENODATA || transfer.completed != 0) {
        return CoordinateReadFailure::ShortRead;
    }
    return fallback;
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

CoordinateDecryptError ContextStatusError(int status, bool ptrace) {
    if (!ptrace &&
        (status == -EPROTO || status == -EINVAL || status == -EMSGSIZE ||
         status == -E2BIG || status == -ENOTTY)) {
        return CoordinateDecryptError::ContextDeviceProtocolMismatch;
    }
    if (status == -ENOENT || status == -ESRCH || status == -EAGAIN) {
        return CoordinateDecryptError::ContextThreadMissing;
    }
    if (status == -EACCES || status == -EPERM) {
        return CoordinateDecryptError::ContextPermissionDenied;
    }
    if (status == -ENODATA) {
        return CoordinateDecryptError::ContextDataInvalid;
    }
    return ptrace
        ? CoordinateDecryptError::PtraceExecutionFailed
        : CoordinateDecryptError::ContextReadFailed;
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
#if 0
    enum class ExecutionBreakpointBackend : std::uint8_t {
        None,
        Kernel,
        Perf,
    };
#endif

    struct PtraceOracleRootSnapshot {
        std::uintptr_t bridge = 0;
        std::uintptr_t entry = 0;
        std::uintptr_t mappingStart = 0;
        std::uintptr_t mappingEnd = 0;
        std::size_t entryWindowSize = 0;
        std::uint64_t entryFingerprint = 0;

        bool operator==(
            const PtraceOracleRootSnapshot& other) const noexcept {
            return bridge == other.bridge && entry == other.entry &&
                mappingStart == other.mappingStart &&
                mappingEnd == other.mappingEnd &&
                entryWindowSize == other.entryWindowSize &&
                entryFingerprint == other.entryFingerprint;
        }
    };

    struct PtraceOracleCacheKey {
        PtraceOracleRootSnapshot root{};
        std::uintptr_t oracleWindowStart = 0;
        std::size_t oracleWindowSize = 0;
        std::uint64_t oracleWindowFingerprint = 0;
        bool operandsResolved = false;

        bool IsValid() const noexcept {
            return root.entry != 0 && root.entryFingerprint != 0 &&
                oracleWindowStart != 0 && oracleWindowSize != 0 &&
                oracleWindowFingerprint != 0;
        }

        std::uint64_t CodeFingerprint() const noexcept {
            std::uint64_t hash = kOracleFingerprintOffset;
            hash = HashOracleValue(hash, root.bridge);
            hash = HashOracleValue(hash, root.entry);
            hash = HashOracleValue(hash, root.mappingStart);
            hash = HashOracleValue(hash, root.mappingEnd);
            hash = HashOracleValue(hash, root.entryWindowSize);
            hash = HashOracleValue(hash, root.entryFingerprint);
            hash = HashOracleValue(hash, oracleWindowStart);
            hash = HashOracleValue(hash, oracleWindowSize);
            hash = HashOracleValue(hash, oracleWindowFingerprint);
            return hash != 0 ? hash : UINT64_C(1);
        }
    };

    MemoryTransportMode mode = MemoryTransportMode::ProcessVm;
    pid_t processId = -1;
    paradise_driver* kernel = nullptr;
    std::unique_ptr<KernelRpcTransport> privateRpc;
    kernel_rpc_abi::ProcessHandle privateProcessHandle =
        kernel_rpc_abi::kInvalidProcessHandle;
#if 0
    PerfExecutionBreakpoint perfExecutionBreakpoint;
    ExecutionBreakpointBackend executionBreakpointBackend =
        ExecutionBreakpointBackend::None;
    bool executionBreakpointConfigured = false;
    std::uintptr_t executionBreakpointAddress = 0;
    std::array<hwbp_record, HWBP_MAX_RECORDS>
        executionBreakpointRecordBuffer{};
#endif
    int processMemFd = -1;
    int threadContextFd = -1;
    std::unique_ptr<ThreadContextDeviceTransport> threadContextTransport;
    std::unique_ptr<ThreadExecutionContextProvider> threadContextProvider;
    PtracePacgaOracleReader ptraceContextReader;
    std::unique_ptr<PtraceExecutionContextProvider> ptraceContextProvider;
    PacgaOracleInstruction ptraceOracleInstruction{};
    PtraceOracleCacheKey ptraceOracleCacheKey{};
    ProcessExecutionContextSource executionContextSource =
        ProcessExecutionContextSource::None;
    ProcessExecutionContextDiagnostic executionContextDiagnostic{};
    int lastThreadContextOpenStatus = 0;
    bool ptraceOracleOperandsResolved = false;
    CoordinateDecryptError ptraceOracleFailure =
        CoordinateDecryptError::PtraceOracleResolveFailed;
    int ptraceOracleSystemError = 0;
    std::chrono::steady_clock::time_point nextThreadContextOpen{};
    std::chrono::steady_clock::time_point nextPtraceOracleResolve{};
    CoordinateReplayTransportLayout coordinateReplayLayout{};
    bool coordinateBatchDisabled = false;
    bool open = false;
#if LENGJING_ENABLE_PROJECTILE_TRACKING
    bool writable = false;
#endif
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
        ptraceOracleCacheKey = {};
        executionContextSource = ProcessExecutionContextSource::None;
        executionContextDiagnostic = {};
        lastThreadContextOpenStatus = 0;
        ptraceOracleOperandsResolved = false;
        ptraceOracleFailure =
            CoordinateDecryptError::PtraceOracleResolveFailed;
        ptraceOracleSystemError = 0;
        nextThreadContextOpen = {};
        nextPtraceOracleResolve = {};
        coordinateReplayLayout = {};
        coordinateBatchDisabled = false;
#if 0
        static_cast<void>(RemoveExecutionBreakpointsUnlocked());
#endif
        if (privateRpc != nullptr &&
            privateProcessHandle != kernel_rpc_abi::kInvalidProcessHandle) {
            static_cast<void>(privateRpc->ReleaseProcessHandle(
                privateProcessHandle));
        }
        privateRpc.reset();
        privateProcessHandle = kernel_rpc_abi::kInvalidProcessHandle;
        if (processMemFd >= 0) {
            ::close(processMemFd);
            processMemFd = -1;
        }
        delete kernel;
        kernel = nullptr;
        mode = MemoryTransportMode::ProcessVm;
        processId = -1;
        open = false;
#if LENGJING_ENABLE_PROJECTILE_TRACKING
        writable = false;
#endif
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
        if (threadContextFd < 0) {
            lastThreadContextOpenStatus = errno != 0 ? -errno : -EIO;
            return false;
        }
        lastThreadContextOpenStatus = 0;

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
        ptraceOracleFailure =
            CoordinateDecryptError::PtraceOracleResolveFailed;
        ptraceOracleSystemError = 0;
        ptraceOracleOperandsResolved = false;

        const std::uintptr_t moduleBase =
            FindMappedModuleBase(processId, "libUE4.so");
        if (!IsRemoteRangeValid(moduleBase, 4) ||
            coordinateReplayLayout.rootRva >
                std::numeric_limits<std::uintptr_t>::max() - moduleBase ||
            coordinateReplayLayout.bridgeOffset >
                std::numeric_limits<std::uintptr_t>::max() - moduleBase -
                    coordinateReplayLayout.rootRva) {
            ptraceOracleInstruction = {};
            ptraceOracleCacheKey = {};
            nextPtraceOracleResolve = {};
            ptraceOracleFailure = CoordinateDecryptError::RootReadFailed;
            ptraceOracleSystemError = -ERANGE;
            return false;
        }

        const auto readCoordinate = [this](std::uintptr_t address,
                                           void* destination,
                                           std::size_t size) {
            CoordinateReadDiagnostic diagnostic{};
            const bool read = ReadCoordinateMemoryUnlocked(
                address, destination, size, diagnostic);
            if (!read) ptraceOracleSystemError = diagnostic.systemError;
            return read;
        };

        const auto invalidateCache = [this]() {
            ptraceOracleInstruction = {};
            ptraceOracleCacheKey = {};
            nextPtraceOracleResolve = {};
        };

        const auto readRootPointers = [&](std::uintptr_t& bridge,
                                          std::uintptr_t& entry) {
            bridge = 0;
            entry = 0;
            std::uint64_t rawBridge = 0;
            if (!readCoordinate(
                    moduleBase + coordinateReplayLayout.rootRva +
                        coordinateReplayLayout.bridgeOffset,
                    &rawBridge,
                    sizeof(rawBridge))) {
                ptraceOracleFailure = CoordinateDecryptError::RootReadFailed;
                return false;
            }
            bridge = static_cast<std::uintptr_t>(
                rawBridge & kPointerPayloadMask);
            if (bridge > std::numeric_limits<std::uintptr_t>::max() -
                    coordinateReplayLayout.entryOffset ||
                !IsRemoteRangeValid(
                    bridge + coordinateReplayLayout.entryOffset,
                    sizeof(std::uint64_t))) {
                ptraceOracleFailure = CoordinateDecryptError::RootReadFailed;
                ptraceOracleSystemError = -ERANGE;
                return false;
            }

            std::uint64_t rawEntry = 0;
            if (!readCoordinate(
                    bridge + coordinateReplayLayout.entryOffset,
                    &rawEntry,
                    sizeof(rawEntry))) {
                ptraceOracleFailure = CoordinateDecryptError::RootReadFailed;
                return false;
            }
            entry = static_cast<std::uintptr_t>(
                rawEntry & kPointerPayloadMask);
            if (!IsRemoteRangeValid(entry, sizeof(std::uint32_t))) {
                ptraceOracleFailure = CoordinateDecryptError::RootReadFailed;
                ptraceOracleSystemError = -ERANGE;
                return false;
            }
            return true;
        };

        const auto readFingerprint = [&](std::uintptr_t address,
                                         std::size_t size,
                                         std::uint64_t& fingerprint) {
            fingerprint = 0;
            if (size == 0 ||
                size > kOracleMaximumFingerprintWindowSize) {
                ptraceOracleSystemError = -ERANGE;
                return false;
            }
            std::array<std::uint8_t,
                       kOracleMaximumFingerprintWindowSize> bytes{};
            if (!readCoordinate(address, bytes.data(), size)) return false;
            fingerprint = HashOracleBytes(bytes.data(), size);
            if (fingerprint == 0) fingerprint = UINT64_C(1);
            return true;
        };

        const auto readStableRoot = [&](PtraceOracleRootSnapshot& snapshot) {
            snapshot = {};
            for (int attempt = 0;
                 attempt < kOracleStableSnapshotAttempts;
                 ++attempt) {
                std::uintptr_t bridgeBefore = 0;
                std::uintptr_t entryBefore = 0;
                if (!readRootPointers(bridgeBefore, entryBefore)) return false;

                std::uintptr_t mappingStart = 0;
                std::uintptr_t mappingEnd = 0;
                if (!FindExecutableMapping(
                        processId,
                        entryBefore,
                        mappingStart,
                        mappingEnd)) {
                    ptraceOracleFailure =
                        CoordinateDecryptError::EntryMappingMissing;
                    ptraceOracleSystemError = -ENOENT;
                    return false;
                }
                const std::size_t entryWindowSize =
                    std::min<std::size_t>(
                        kOracleEntryFingerprintSize,
                        static_cast<std::size_t>(mappingEnd - entryBefore));
                std::uint64_t entryFingerprint = 0;
                if (!readFingerprint(
                        entryBefore,
                        entryWindowSize,
                        entryFingerprint)) {
                    return false;
                }

                std::uintptr_t bridgeAfter = 0;
                std::uintptr_t entryAfter = 0;
                if (!readRootPointers(bridgeAfter, entryAfter)) return false;
                if (bridgeBefore == bridgeAfter &&
                    entryBefore == entryAfter) {
                    snapshot = {
                        bridgeBefore,
                        entryBefore,
                        mappingStart,
                        mappingEnd,
                        entryWindowSize,
                        entryFingerprint,
                    };
                    return true;
                }
            }
            ptraceOracleFailure = CoordinateDecryptError::RootReadFailed;
            ptraceOracleSystemError = -EAGAIN;
            return false;
        };

        const auto scan = [&readCoordinate](std::uintptr_t begin,
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
                if (!readCoordinate(cursor, bytes.data(), size)) return false;
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

        for (int attempt = 0;
             attempt < kOracleStableSnapshotAttempts;
             ++attempt) {
            PtraceOracleRootSnapshot root{};
            if (!readStableRoot(root)) {
                invalidateCache();
                return false;
            }

            const auto now = std::chrono::steady_clock::now();
            if (nextPtraceOracleResolve.time_since_epoch().count() != 0 &&
                now < nextPtraceOracleResolve &&
                ptraceOracleInstruction.IsValid() &&
                ptraceOracleCacheKey.IsValid() &&
                ptraceOracleCacheKey.root == root) {
                std::uint64_t currentOracleFingerprint = 0;
                if (!readFingerprint(
                        ptraceOracleCacheKey.oracleWindowStart,
                        ptraceOracleCacheKey.oracleWindowSize,
                        currentOracleFingerprint)) {
                    invalidateCache();
                    return false;
                }
                PtraceOracleRootSnapshot verifiedRoot{};
                if (!readStableRoot(verifiedRoot)) {
                    invalidateCache();
                    return false;
                }
                if (!(verifiedRoot == root)) {
                    invalidateCache();
                    ptraceOracleFailure =
                        CoordinateDecryptError::PtraceOracleResolveFailed;
                    ptraceOracleSystemError = -EAGAIN;
                    continue;
                }
                if (currentOracleFingerprint ==
                    ptraceOracleCacheKey.oracleWindowFingerprint) {
                    instruction = ptraceOracleInstruction;
                    ptraceOracleOperandsResolved =
                        ptraceOracleCacheKey.operandsResolved;
                    ptraceOracleFailure = CoordinateDecryptError::None;
                    ptraceOracleSystemError = 0;
                    return true;
                }
                invalidateCache();
            } else if (ptraceOracleInstruction.IsValid() ||
                       ptraceOracleCacheKey.IsValid()) {
                invalidateCache();
            }

            std::uintptr_t found = 0;
            const std::uintptr_t preferredEnd = std::min(
                root.mappingEnd,
                root.entry <=
                        std::numeric_limits<std::uintptr_t>::max() - 0x5000U
                    ? root.entry + 0x5000U
                    : root.mappingEnd);
            if (!scan(root.entry, preferredEnd, found) &&
                !scan(root.mappingStart, root.mappingEnd, found)) {
                invalidateCache();
                return false;
            }

            PacgaOperands operands{
                coordinateReplayLayout.pacgaData,
                coordinateReplayLayout.pacgaModifier,
            };
            const std::uintptr_t operandWindowStart = std::max(
                root.mappingStart,
                found >= kOracleImmediateWindowInstructions *
                        sizeof(std::uint32_t)
                    ? found - kOracleImmediateWindowInstructions *
                        sizeof(std::uint32_t)
                    : root.mappingStart);
            const std::size_t operandInstructionCount =
                static_cast<std::size_t>(
                    (found - operandWindowStart) /
                    sizeof(std::uint32_t)) +
                1U;
            std::array<std::uint32_t,
                       kOracleImmediateWindowInstructions + 1U>
                operandInstructions{};
            if (operandInstructionCount > operandInstructions.size() ||
                !readCoordinate(
                    operandWindowStart,
                    operandInstructions.data(),
                    operandInstructionCount * sizeof(std::uint32_t))) {
                invalidateCache();
                return false;
            }
            const std::size_t operandWindowSize =
                operandInstructionCount * sizeof(std::uint32_t);
            std::uint64_t operandFingerprint = HashOracleBytes(
                operandInstructions.data(), operandWindowSize);
            if (operandFingerprint == 0) operandFingerprint = UINT64_C(1);

            PacgaOperands resolvedOperands{};
            const bool operandsResolved =
                ResolvePacgaOperandsFromImmediateBlock(
                    operandInstructions.data(),
                    operandInstructionCount,
                    operandInstructionCount - 1U,
                    resolvedOperands);
            if (operandsResolved) operands = resolvedOperands;

            PtraceOracleRootSnapshot verifiedRootBefore{};
            PtraceOracleRootSnapshot verifiedRootAfter{};
            std::uint64_t verifiedOperandFingerprint = 0;
            if (!readStableRoot(verifiedRootBefore) ||
                !readFingerprint(
                    operandWindowStart,
                    operandWindowSize,
                    verifiedOperandFingerprint) ||
                !readStableRoot(verifiedRootAfter)) {
                invalidateCache();
                return false;
            }
            if (!(verifiedRootBefore == root) ||
                !(verifiedRootAfter == root) ||
                verifiedOperandFingerprint != operandFingerprint) {
                invalidateCache();
                ptraceOracleFailure =
                    CoordinateDecryptError::PtraceOracleResolveFailed;
                ptraceOracleSystemError = -EAGAIN;
                continue;
            }

            ptraceOracleCacheKey = {
                verifiedRootAfter,
                operandWindowStart,
                operandWindowSize,
                verifiedOperandFingerprint,
                operandsResolved,
            };
            ptraceOracleInstruction = {
                found,
                operands.data,
                operands.modifier,
                {
                    verifiedRootAfter.entry,
                    ptraceOracleCacheKey.CodeFingerprint(),
                },
            };
            instruction = ptraceOracleInstruction;
            ptraceOracleOperandsResolved = operandsResolved;
            ptraceOracleFailure = CoordinateDecryptError::None;
            ptraceOracleSystemError = 0;
            nextPtraceOracleResolve = now + std::chrono::seconds(1);
            return true;
        }

        invalidateCache();
        ptraceOracleFailure =
            CoordinateDecryptError::PtraceOracleResolveFailed;
        ptraceOracleSystemError = -EAGAIN;
        return false;
    }

    void Close() noexcept {
        std::lock_guard<std::mutex> lock(ioMutex);
        ResetUnlocked();
    }

    bool Open(int modeIndex,
              pid_t targetProcessId,
              std::string_view processName,
              RuntimeDiagnostic& diagnostic,
              std::string& error) {
        std::lock_guard<std::mutex> lock(ioMutex);
        ResetUnlocked();
        diagnostic = {};
        if (!IsValidMemoryTransportMode(modeIndex) || targetProcessId <= 0) {
            error = "内存读取模式参数无效";
            diagnostic = {
                RuntimeError::MemoryModeInvalid,
                -EINVAL,
            };
            return false;
        }

        mode = static_cast<MemoryTransportMode>(modeIndex);
        processId = targetProcessId;
        switch (mode) {
            case MemoryTransportMode::ProcessVm:
#if LENGJING_ENABLE_PROJECTILE_TRACKING
                writable = true;
#endif
                break;
            case MemoryTransportMode::KernelDriver:
                if (!EnsureKernelDriverReady(error)) {
                    diagnostic.error =
                        RuntimeError::KernelDriverSetupFailed;
                    ResetUnlocked();
                    return false;
                }
                kernel = new (std::nothrow) paradise_driver();
                if (kernel == nullptr) {
                    error = "无法创建内核读取接口";
                    diagnostic = {
                        RuntimeError::KernelInterfaceCreateFailed,
                        -ENOMEM,
                    };
                    ResetUnlocked();
                    return false;
                }
                kernel->initialize(processId);
#if LENGJING_ENABLE_PROJECTILE_TRACKING
                writable = true;
#endif
                break;
            case MemoryTransportMode::PrivateRpc: {
                privateRpc.reset(new (std::nothrow) KernelRpcTransport());
                if (privateRpc == nullptr) {
                    error = "无法创建内核 RPC 接口";
                    diagnostic = {
                        RuntimeError::PrivateRpcCreateFailed,
                        -ENOMEM,
                    };
                    ResetUnlocked();
                    return false;
                }
                std::int32_t resolvedProcessId = -1;
                const int result = privateRpc->FindProcessId(
                    processName, resolvedProcessId);
                if (result != 0) {
                    error = "内核 RPC 接口不可用: " +
                        RpcFailureText(result);
                    diagnostic = {
                        RuntimeError::PrivateRpcProcessLookupFailed,
                        result,
                    };
                    ResetUnlocked();
                    return false;
                }
                if (resolvedProcessId != processId) {
                    error = "内核 RPC 返回的进程标识不匹配";
                    diagnostic = {
                        RuntimeError::PrivateRpcProcessMismatch,
                        -ESRCH,
                    };
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
                    diagnostic = {
                        attachResult == -ENOTSUP
                            ? RuntimeError::PrivateRpcAttachUnsupported
                            : RuntimeError::PrivateRpcAttachFailed,
                        attachResult,
                    };
                    ResetUnlocked();
                    return false;
                }
#if LENGJING_ENABLE_PROJECTILE_TRACKING
                writable = true;
#endif
                break;
            }
            case MemoryTransportMode::Count:
            default:
                error = "内存读取模式参数无效";
                diagnostic = {
                    RuntimeError::MemoryModeInvalid,
                    -EINVAL,
                };
                ResetUnlocked();
                return false;
        }

        static_cast<void>(OpenThreadContextUnlocked());
        open = true;
        diagnostic = {};
        error.clear();
        return true;
    }

    int OpenProcessMemUnlocked() {
        if (processMemFd >= 0) return 0;
        const std::string path =
            "/proc/" + std::to_string(processId) + "/mem";
        errno = 0;
        processMemFd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        return processMemFd >= 0
            ? 0
            : (errno != 0 ? -errno : -EIO);
    }

    bool AttemptCoordinateReadUnlocked(
        CoordinateReadPath path,
        std::uintptr_t address,
        void* destination,
        std::size_t size,
        CoordinateReadDiagnostic& diagnostic) {
        const bool primaryAttempt = diagnostic.attemptCount == 0;
        if (platform::PerformanceTraceEnabled()) {
            platform::RecordPerformanceCount(
                platform::PerformanceCounter::CoordinatePathAttempts);
            if (!primaryAttempt) {
                platform::RecordPerformanceCount(
                    platform::PerformanceCounter::CoordinateFallbacks);
            }
        }
        diagnostic.lastPath = path;
        diagnostic.attemptedPaths |= CoordinateReadPathMask(path);
        ++diagnostic.attemptCount;

        MemoryTransferResult transfer{};
        CoordinateReadFailure fallback =
            CoordinateReadFailure::TransportUnavailable;
        switch (path) {
            case CoordinateReadPath::ProcessVm:
                fallback = CoordinateReadFailure::ProcessVmReadFailed;
                transfer = ProcessVmTransferExact(
                    processId, address, destination, size, false);
                break;
            case CoordinateReadPath::ProcMem: {
                const int openStatus = OpenProcessMemUnlocked();
                if (openStatus != 0) {
                    transfer.status = openStatus;
                    fallback = CoordinateReadFailure::ProcMemOpenFailed;
                } else {
                    transfer = ProcessMemReadExact(
                        processMemFd, address, destination, size);
                    fallback = CoordinateReadFailure::ProcMemReadFailed;
                }
                break;
            }
            case CoordinateReadPath::None:
                transfer.status = -EINVAL;
                break;
        }

        if (primaryAttempt) {
            diagnostic.primaryCompleted = transfer.completed;
            diagnostic.primarySystemError = transfer.status;
        }
        diagnostic.lastCompleted = transfer.completed;
        diagnostic.lastSystemError = transfer.status;
        diagnostic.systemError = transfer.status;
        diagnostic.failure =
            ClassifyCoordinateReadFailure(fallback, transfer);
        if (transfer.status != 0 || transfer.completed != size) return false;
        diagnostic.failure = CoordinateReadFailure::None;
        diagnostic.systemError = 0;
        return true;
    }

    bool AttemptCoordinateBatchReadUnlocked(
        CoordinateReadPath path,
        const MemoryReadRequest* requests,
        std::size_t count,
        std::size_t totalSize,
        CoordinateReadDiagnostic& diagnostic,
        std::size_t& failedIndex) {
        const bool primaryAttempt = diagnostic.attemptCount == 0;
        if (platform::PerformanceTraceEnabled()) {
            platform::RecordPerformanceCount(
                platform::PerformanceCounter::CoordinatePathAttempts);
            if (!primaryAttempt) {
                platform::RecordPerformanceCount(
                    platform::PerformanceCounter::CoordinateFallbacks);
            }
        }
        diagnostic.lastPath = path;
        diagnostic.attemptedPaths |= CoordinateReadPathMask(path);
        ++diagnostic.attemptCount;

        MemoryBatchTransferResult transfer{};
        CoordinateReadFailure fallback =
            CoordinateReadFailure::TransportUnavailable;
        switch (path) {
            case CoordinateReadPath::ProcessVm:
                fallback = CoordinateReadFailure::ProcessVmReadFailed;
                transfer = ProcessVmReadBatchOnce(
                    processId, requests, count, totalSize);
                if (IsStructuralCoordinateBatchFailure(transfer.status)) {
                    coordinateBatchDisabled = true;
                }
                break;
            case CoordinateReadPath::ProcMem: {
                const int openStatus = OpenProcessMemUnlocked();
                if (openStatus != 0) {
                    transfer.status = openStatus;
                    transfer.failedIndex = 0;
                    fallback = CoordinateReadFailure::ProcMemOpenFailed;
                } else {
                    transfer = ProcessMemReadBatchExact(
                        processMemFd, requests, count);
                    fallback = CoordinateReadFailure::ProcMemReadFailed;
                }
                break;
            }
            case CoordinateReadPath::None:
                transfer.status = -EINVAL;
                break;
        }

        const bool succeeded = transfer.status == 0 &&
            transfer.completed == totalSize && transfer.failedIndex == count;
        if (!succeeded && transfer.status == 0) {
            transfer.status = -ENODATA;
        }
        const std::size_t completed = succeeded
            ? totalSize
            : transfer.completed;
        if (primaryAttempt) {
            diagnostic.primaryCompleted = completed;
            diagnostic.primarySystemError = transfer.status;
        }
        diagnostic.lastCompleted = completed;
        diagnostic.lastSystemError = transfer.status;
        diagnostic.systemError = transfer.status;
        if (succeeded) {
            failedIndex = count;
            diagnostic.batchFailedIndex = count;
            diagnostic.failedItemCompleted = 0;
            diagnostic.address = requests[0].remoteAddress;
            diagnostic.size = totalSize;
            diagnostic.failure = CoordinateReadFailure::None;
            diagnostic.systemError = 0;
            return true;
        }

        failedIndex = transfer.failedIndex < count
            ? transfer.failedIndex
            : count - 1;
        diagnostic.batchFailedIndex = failedIndex;
        diagnostic.failedItemCompleted = transfer.failedCompleted;
        diagnostic.address = requests[failedIndex].remoteAddress;
        diagnostic.size = requests[failedIndex].size;
        diagnostic.failure = ClassifyCoordinateReadFailure(
            fallback,
            MemoryTransferResult{
                transfer.status,
                transfer.failedCompleted,
            });
        return false;
    }

    bool ReadCoordinateMemoryCoreUnlocked(
        std::uintptr_t address,
        void* destination,
        std::size_t size,
        CoordinateReadDiagnostic& diagnostic) {
        diagnostic = {};
        diagnostic.address = address;
        diagnostic.size = size;
        if (!open) {
            diagnostic.failure =
                CoordinateReadFailure::TransportUnavailable;
            diagnostic.systemError = -ENODEV;
            return false;
        }
        if (destination == nullptr || !IsRemoteRangeValid(address, size)) {
            diagnostic.failure = CoordinateReadFailure::InvalidRange;
            diagnostic.systemError = -EINVAL;
            return false;
        }

        diagnostic.primaryPath = kCoordinateReadPathOrder.front();
        return TryCoordinateReadPaths([&](CoordinateReadPath path) {
            return AttemptCoordinateReadUnlocked(
                path, address, destination, size, diagnostic);
        });
    }

    bool ReadCoordinateMemoryUnlocked(
        std::uintptr_t address,
        void* destination,
        std::size_t size,
        CoordinateReadDiagnostic& diagnostic) {
        if (!platform::PerformanceTraceEnabled()) {
            return ReadCoordinateMemoryCoreUnlocked(
                address, destination, size, diagnostic);
        }
        platform::PerformanceTraceScope trace(
            platform::PerformancePhase::CoordinateRemoteRead, 64);
        const bool read = ReadCoordinateMemoryCoreUnlocked(
            address, destination, size, diagnostic);
        platform::RecordPerformanceRead(
            platform::PerformanceReadKind::Coordinate,
            size,
            read);
        return read;
    }

    bool ReadCoordinateMemoryBatchCoreUnlocked(
        const MemoryReadRequest* requests,
        std::size_t count,
        CoordinateReadDiagnostic& diagnostic,
        std::size_t& failedIndex) {
        diagnostic = {};
        failedIndex = 0;
        if (requests != nullptr && count != 0) {
            diagnostic.address = requests[0].remoteAddress;
            diagnostic.size = requests[0].size;
        }
        diagnostic.batchItemCount = count;
        diagnostic.batchFailedIndex = 0;
        if (!open) {
            diagnostic.failure =
                CoordinateReadFailure::TransportUnavailable;
            diagnostic.systemError = -ENODEV;
            return false;
        }
        if (requests == nullptr || count == 0 ||
            count > kCoordinateMemoryBatchRequestLimit) {
            diagnostic.failure = CoordinateReadFailure::InvalidRange;
            diagnostic.systemError = -EINVAL;
            return false;
        }

        std::size_t totalSize = 0;
        const std::size_t maximumTransferSize = static_cast<std::size_t>(
            std::numeric_limits<ssize_t>::max());
        for (std::size_t index = 0; index < count; ++index) {
            const MemoryReadRequest& request = requests[index];
            if (request.localBuffer == nullptr ||
                !IsRemoteRangeValid(request.remoteAddress, request.size) ||
                request.size > maximumTransferSize - totalSize) {
                failedIndex = index;
                diagnostic.batchFailedIndex = index;
                diagnostic.address = request.remoteAddress;
                diagnostic.size = request.size;
                diagnostic.failure = CoordinateReadFailure::InvalidRange;
                diagnostic.systemError = -EINVAL;
                return false;
            }
            totalSize += request.size;
        }

        diagnostic.address = requests[0].remoteAddress;
        diagnostic.size = totalSize;
        if (coordinateBatchDisabled) {
            for (std::size_t index = 0; index < count; ++index) {
                CoordinateReadDiagnostic itemDiagnostic{};
                if (!ReadCoordinateMemoryCoreUnlocked(
                        requests[index].remoteAddress,
                        requests[index].localBuffer,
                        requests[index].size,
                        itemDiagnostic)) {
                    diagnostic = itemDiagnostic;
                    diagnostic.batchItemCount = count;
                    diagnostic.batchFailedIndex = index;
                    diagnostic.failedItemCompleted =
                        itemDiagnostic.lastCompleted;
                    failedIndex = index;
                    return false;
                }
            }
            diagnostic = {};
            diagnostic.batchItemCount = count;
            diagnostic.batchFailedIndex = count;
            failedIndex = count;
            return true;
        }
        diagnostic.primaryPath = kCoordinateReadPathOrder.front();
        return TryCoordinateReadPaths([&](CoordinateReadPath path) {
            return AttemptCoordinateBatchReadUnlocked(
                path,
                requests,
                count,
                totalSize,
                diagnostic,
                failedIndex);
        });
    }

    bool ReadCoordinateMemoryBatchUnlocked(
        const MemoryReadRequest* requests,
        std::size_t count,
        CoordinateReadDiagnostic& diagnostic,
        std::size_t& failedIndex) {
        if (!platform::PerformanceTraceEnabled()) {
            return ReadCoordinateMemoryBatchCoreUnlocked(
                requests, count, diagnostic, failedIndex);
        }
        platform::PerformanceTraceScope trace(
            platform::PerformancePhase::CoordinateRemoteRead, 64);
        const std::size_t requestedBytes =
            CoordinateBatchRequestedBytes(requests, count);
        const bool read = ReadCoordinateMemoryBatchCoreUnlocked(
            requests, count, diagnostic, failedIndex);
        platform::RecordPerformanceRead(
            platform::PerformanceReadKind::Coordinate,
            requestedBytes,
            read,
            count);
        return read;
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
                processId, address, destination, size, false).status == 0;
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
        if (!platform::PerformanceTraceEnabled()) {
            std::lock_guard<std::mutex> lock(ioMutex);
            return ReadUnlocked(address, destination, size);
        }
        platform::PerformanceTraceScope trace(
            platform::PerformancePhase::RemoteRead, 64);
        std::lock_guard<std::mutex> lock(ioMutex);
        const bool read = ReadUnlocked(address, destination, size);
        platform::RecordPerformanceRead(
            platform::PerformanceReadKind::Standard,
            size,
            read);
        return read;
    }

#if 0
    bool SupportsExecutionBreakpoints() const noexcept {
        std::lock_guard<std::mutex> lock(ioMutex);
        if (!open || processId <= 0) {
            return false;
        }
        if (mode == MemoryTransportMode::KernelDriver &&
            kernel != nullptr) {
            std::uint64_t breakpointCount = 0;
            std::uint64_t watchpointCount = 0;
            try {
                if (kernel->hwbp_get_info(
                        &breakpointCount, &watchpointCount) &&
                    breakpointCount != 0) {
                    return true;
                }
            } catch (...) {
            }
        }
        return PerfExecutionBreakpoint::IsSupported();
    }

    bool ConfigureExecutionBreakpoint(std::uintptr_t address) noexcept {
        std::lock_guard<std::mutex> lock(ioMutex);
        if (!open || processId <= 0 ||
            !IsRemoteRangeValid(address, 4) || (address & 3U) != 0) {
            return false;
        }

        if (executionBreakpointConfigured &&
            executionBreakpointAddress == address) {
            return true;
        }
        if (!RemoveExecutionBreakpointsUnlocked()) return false;

        bool kernelBreakpointAvailable = false;
        if (mode == MemoryTransportMode::KernelDriver &&
            kernel != nullptr) {
            std::uint64_t breakpointCount = 0;
            std::uint64_t watchpointCount = 0;
            try {
                kernelBreakpointAvailable = kernel->hwbp_get_info(
                        &breakpointCount, &watchpointCount) &&
                    breakpointCount != 0;
            } catch (...) {
                kernelBreakpointAvailable = false;
            }
        }

        if (kernelBreakpointAvailable) {
            hwbp_point_config point{};
            point.bt = HWBP_BREAKPOINT_X;
            point.bl = HWBP_BREAKPOINT_LEN_4;
            point.bs = SCOPE_ALL_THREADS;
            point.hit_addr = static_cast<std::uint64_t>(address);
            try {
                if (kernel->hwbp_set(processId, &point, 1)) {
                    executionBreakpointBackend =
                        ExecutionBreakpointBackend::Kernel;
                    executionBreakpointConfigured = true;
                    executionBreakpointAddress = address;
                    return true;
                }
                static_cast<void>(kernel->hwbp_remove(processId));
            } catch (...) {
                try {
                    static_cast<void>(kernel->hwbp_remove(processId));
                } catch (...) {
                }
            }
        }

        if (!PerfExecutionBreakpoint::IsSupported() ||
            !perfExecutionBreakpoint.Configure(processId, address)) {
            return false;
        }
        executionBreakpointBackend =
            ExecutionBreakpointBackend::Perf;
        executionBreakpointConfigured = true;
        executionBreakpointAddress = address;
        return true;
    }

    bool ReadExecutionBreakpointRecords(
        ExecutionBreakpointRecord* records,
        std::size_t capacity,
        std::size_t& recordsRead,
        std::uintptr_t& hitAddress,
        std::size_t& totalRecords) noexcept {
        recordsRead = 0;
        hitAddress = 0;
        totalRecords = 0;
        std::lock_guard<std::mutex> lock(ioMutex);
        if (!open || processId <= 0 || !executionBreakpointConfigured ||
            records == nullptr || capacity == 0) {
            return false;
        }

        if (executionBreakpointBackend ==
            ExecutionBreakpointBackend::Perf) {
            return perfExecutionBreakpoint.ReadRecords(
                records,
                capacity,
                recordsRead,
                hitAddress,
                totalRecords);
        }
        if (executionBreakpointBackend !=
                ExecutionBreakpointBackend::Kernel ||
            mode != MemoryTransportMode::KernelDriver ||
            kernel == nullptr) {
            return false;
        }

        const std::size_t requested = std::min(
            capacity, static_cast<std::size_t>(HWBP_MAX_RECORDS));
        std::uint64_t nativeHitAddress = 0;
        int nativeTotalRecords = 0;
        try {
            const int copied = kernel->hwbp_read_records(
                processId,
                0,
                executionBreakpointRecordBuffer.data(),
                static_cast<int>(requested),
                &nativeHitAddress,
                &nativeTotalRecords);
            if (copied < 0 || static_cast<std::size_t>(copied) > requested ||
                nativeTotalRecords < 0 ||
                nativeTotalRecords > HWBP_MAX_RECORDS) {
                return false;
            }
            for (int index = 0; index < copied; ++index) {
                const hwbp_record& source =
                    executionBreakpointRecordBuffer[
                        static_cast<std::size_t>(index)];
                records[static_cast<std::size_t>(index)] = {
                    source.tid,
                    source.hit_count,
                    static_cast<std::uintptr_t>(source.pc),
                    static_cast<std::uintptr_t>(source.sp),
                    static_cast<std::uintptr_t>(source.x0),
                    static_cast<std::uintptr_t>(source.x23),
                };
            }
            recordsRead = static_cast<std::size_t>(copied);
            hitAddress = static_cast<std::uintptr_t>(nativeHitAddress);
            totalRecords = static_cast<std::size_t>(nativeTotalRecords);
            return true;
        } catch (...) {
            recordsRead = 0;
            hitAddress = 0;
            totalRecords = 0;
            return false;
        }
    }

    bool RemoveExecutionBreakpointsUnlocked() noexcept {
        if (!executionBreakpointConfigured) return true;
        bool removed = true;
        if (executionBreakpointBackend ==
            ExecutionBreakpointBackend::Kernel) {
            if (kernel == nullptr || processId <= 0) {
                removed = false;
            } else {
                try {
                    removed = kernel->hwbp_remove(processId);
                } catch (...) {
                    removed = false;
                }
            }
        } else if (executionBreakpointBackend ==
                   ExecutionBreakpointBackend::Perf) {
            removed = perfExecutionBreakpoint.Remove();
        }
        executionBreakpointBackend =
            ExecutionBreakpointBackend::None;
        executionBreakpointConfigured = false;
        executionBreakpointAddress = 0;
        return removed;
    }

    bool RemoveExecutionBreakpoints() noexcept {
        std::lock_guard<std::mutex> lock(ioMutex);
        return RemoveExecutionBreakpointsUnlocked();
    }
#endif

    bool ReadCoordinateMemory(
        std::uintptr_t address,
        void* destination,
        std::size_t size,
        CoordinateReadDiagnostic& diagnostic) {
        std::lock_guard<std::mutex> lock(ioMutex);
        return ReadCoordinateMemoryUnlocked(
            address, destination, size, diagnostic);
    }

    bool ReadCoordinateMemoryBatch(
        const MemoryReadRequest* requests,
        std::size_t count,
        CoordinateReadDiagnostic& diagnostic,
        std::size_t& failedIndex) {
        std::lock_guard<std::mutex> lock(ioMutex);
        return ReadCoordinateMemoryBatchUnlocked(
            requests, count, diagnostic, failedIndex);
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
        const bool traceEnabled = platform::PerformanceTraceEnabled();
        platform::PerformanceTraceScope trace(
            platform::PerformancePhase::BatchRemoteRead, 16);
        std::uint64_t requestedBytes = 0;
        if (traceEnabled && requests != nullptr) {
            for (std::size_t index = 0; index < count; ++index) {
                requestedBytes += requests[index].size;
            }
        }
        if (traceEnabled) {
            platform::RecordPerformanceRead(
                platform::PerformanceReadKind::Batch,
                requestedBytes,
                true,
                count);
        }
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

#if LENGJING_ENABLE_PROJECTILE_TRACKING
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
                true).status == 0;
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
#endif

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

#if LENGJING_ENABLE_PROJECTILE_TRACKING
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
#endif

    bool ConfigureCoordinateReplay(
        const CoordinateReplayTransportLayout& layout) {
        if (!layout.IsValid()) return false;
        std::lock_guard<std::mutex> lock(ioMutex);
        coordinateReplayLayout = layout;
        ptraceContextProvider.reset();
        ptraceOracleInstruction = {};
        ptraceOracleCacheKey = {};
        ptraceOracleOperandsResolved = false;
        ptraceOracleFailure =
            CoordinateDecryptError::PtraceOracleResolveFailed;
        ptraceOracleSystemError = 0;
        executionContextSource = ProcessExecutionContextSource::None;
        executionContextDiagnostic = {};
        nextPtraceOracleResolve = {};
        return true;
    }

    bool ResolveCoordinateReplayEntry(
        std::uintptr_t moduleBase,
        CoordinateReplayEntrySnapshot& snapshot,
        CoordinateReplayEntryDiagnostic& diagnostic) {
        std::lock_guard<std::mutex> lock(ioMutex);
        snapshot = {};
        diagnostic = {};
        if (!open || !coordinateReplayLayout.IsValid()) {
            diagnostic.error = CoordinateDecryptError::MemoryTransportUnavailable;
            diagnostic.systemError = -ENODEV;
            return false;
        }
        if (!IsRemoteRangeValid(moduleBase, sizeof(std::uint32_t)) ||
            coordinateReplayLayout.rootRva >
                std::numeric_limits<std::uintptr_t>::max() - moduleBase ||
            coordinateReplayLayout.bridgeOffset >
                std::numeric_limits<std::uintptr_t>::max() - moduleBase -
                    coordinateReplayLayout.rootRva) {
            diagnostic.error = CoordinateDecryptError::InvalidConfiguration;
            diagnostic.systemError = -ERANGE;
            return false;
        }

        const std::uintptr_t rootAddress = moduleBase +
            coordinateReplayLayout.rootRva +
            coordinateReplayLayout.bridgeOffset;
        const auto read = [this, &diagnostic](
                              std::uintptr_t address,
                              void* destination,
                              std::size_t size,
                              CoordinateReadStage stage) {
            CoordinateReadDiagnostic current{};
            const bool succeeded = ReadCoordinateMemoryUnlocked(
                address, destination, size, current);
            current.stage = stage;
            if (!succeeded) {
                diagnostic.read = current;
                diagnostic.systemError = current.systemError;
            }
            return succeeded;
        };
        const auto readPointers = [&](std::uintptr_t& bridge,
                                      std::uintptr_t& entry) {
            bridge = 0;
            entry = 0;
            std::uint64_t rawBridge = 0;
            if (!read(
                    rootAddress,
                    &rawBridge,
                    sizeof(rawBridge),
                    CoordinateReadStage::Root)) {
                diagnostic.error = CoordinateDecryptError::RootReadFailed;
                return false;
            }
            bridge = static_cast<std::uintptr_t>(
                rawBridge & kPointerPayloadMask);
            if (!IsRemoteRangeValid(bridge, sizeof(std::uint64_t)) ||
                coordinateReplayLayout.entryOffset >
                    std::numeric_limits<std::uintptr_t>::max() - bridge ||
                !IsRemoteRangeValid(
                    bridge + coordinateReplayLayout.entryOffset,
                    sizeof(std::uint64_t))) {
                diagnostic.error = CoordinateDecryptError::EntryResolveFailed;
                diagnostic.systemError = -ERANGE;
                return false;
            }

            std::uint64_t rawEntry = 0;
            if (!read(
                    bridge + coordinateReplayLayout.entryOffset,
                    &rawEntry,
                    sizeof(rawEntry),
                    CoordinateReadStage::Entry)) {
                diagnostic.error = CoordinateDecryptError::EntryResolveFailed;
                return false;
            }
            entry = static_cast<std::uintptr_t>(
                rawEntry & kPointerPayloadMask);
            if (!IsRemoteRangeValid(entry, sizeof(std::uint32_t)) ||
                (entry & 3U) != 0) {
                diagnostic.error = CoordinateDecryptError::EntryResolveFailed;
                diagnostic.systemError = -ERANGE;
                return false;
            }
            return true;
        };

        constexpr int kStableSnapshotAttempts = 3;
        for (int attempt = 0; attempt < kStableSnapshotAttempts; ++attempt) {
            std::uintptr_t bridgeBefore = 0;
            std::uintptr_t entryBefore = 0;
            if (!readPointers(bridgeBefore, entryBefore)) return false;

            std::uintptr_t mappingStart = 0;
            std::uintptr_t mappingEnd = 0;
            if (!FindExecutableMapping(
                    processId,
                    entryBefore,
                    mappingStart,
                    mappingEnd)) {
                diagnostic.error = CoordinateDecryptError::EntryMappingMissing;
                diagnostic.systemError = -ENOENT;
                diagnostic.snapshot.bridge = bridgeBefore;
                diagnostic.snapshot.entry = entryBefore;
                return false;
            }

            std::uint32_t instruction = 0;
            if (!read(
                    entryBefore,
                    &instruction,
                    sizeof(instruction),
                    CoordinateReadStage::CodePage)) {
                diagnostic.error = CoordinateReadError(
                    diagnostic.read.failure);
                if (diagnostic.error == CoordinateDecryptError::None) {
                    diagnostic.error =
                        CoordinateDecryptError::EntryCodeReadFailed;
                }
                diagnostic.snapshot = {
                    bridgeBefore,
                    entryBefore,
                    mappingStart,
                    mappingEnd,
                    0,
                };
                return false;
            }
            if (instruction == 0 || instruction == UINT32_MAX) {
                diagnostic.error = CoordinateDecryptError::EntryCodeReadFailed;
                diagnostic.systemError = -ENODATA;
                diagnostic.snapshot = {
                    bridgeBefore,
                    entryBefore,
                    mappingStart,
                    mappingEnd,
                    instruction,
                };
                return false;
            }

            std::uintptr_t bridgeAfter = 0;
            std::uintptr_t entryAfter = 0;
            if (!readPointers(bridgeAfter, entryAfter)) return false;
            if (bridgeBefore != bridgeAfter || entryBefore != entryAfter) {
                diagnostic.error = CoordinateDecryptError::EntryMappingChanged;
                diagnostic.systemError = -EAGAIN;
                continue;
            }

            snapshot = {
                bridgeBefore,
                entryBefore,
                mappingStart,
                mappingEnd,
                instruction,
            };
            diagnostic = {};
            diagnostic.snapshot = snapshot;
            return true;
        }

        diagnostic.error = CoordinateDecryptError::EntryMappingChanged;
        diagnostic.systemError = -EAGAIN;
        return false;
    }

    bool ReadProcessExecutionContext(ProcessExecutionContext& context) {
        std::lock_guard<std::mutex> lock(ioMutex);
        context = {};
        executionContextSource = ProcessExecutionContextSource::None;
        executionContextDiagnostic = {};
        if (!open || processId <= 0) {
            executionContextDiagnostic.error =
                CoordinateDecryptError::MemoryTransportUnavailable;
            executionContextDiagnostic.systemError = -ENODEV;
            return false;
        }

        CoordinateDecryptError deviceError =
            CoordinateDecryptError::ContextDeviceOpenFailed;
        int deviceStatus = lastThreadContextOpenStatus;

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
                        ProcessExecutionContextSource::Device;
                    executionContextDiagnostic.source =
                        ProcessExecutionContextSource::Device;
                    return true;
                }
                deviceStatus = -ENODATA;
            } else {
                deviceStatus = refresh.status != 0
                    ? refresh.status
                    : -EIO;
            }
            if (threadContextTransport != nullptr) {
                executionContextDiagnostic.deviceRequestCount =
                    threadContextTransport->Configuration().requestCount;
            }
            deviceError = ContextStatusError(deviceStatus, false);
        } else {
            deviceStatus = lastThreadContextOpenStatus != 0
                ? lastThreadContextOpenStatus
                : -ENODEV;
        }
        executionContextDiagnostic.deviceStatus = deviceStatus;
        executionContextDiagnostic.error = deviceError;
        executionContextDiagnostic.systemError = deviceStatus;

        const auto setPtraceFailure = [this, deviceError, deviceStatus](
                                          CoordinateDecryptError error,
                                          int status) {
            if (deviceError ==
                CoordinateDecryptError::ContextDeviceProtocolMismatch) {
                executionContextDiagnostic.error = deviceError;
                executionContextDiagnostic.systemError = deviceStatus;
                return;
            }
            executionContextDiagnostic.error = error;
            executionContextDiagnostic.systemError = status;
        };

        PacgaOracleInstruction instruction{};
        if (!ResolvePtraceOracleInstructionUnlocked(instruction)) {
            executionContextDiagnostic.pacgaOperandsResolved =
                ptraceOracleOperandsResolved;
            setPtraceFailure(
                ptraceOracleFailure, ptraceOracleSystemError);
            return false;
        }
        executionContextDiagnostic.pacgaOperandsResolved =
            ptraceOracleOperandsResolved;
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
        executionContextDiagnostic.ptraceStatus = refresh.status;
        if (!refresh.HasContext()) {
            const int status = refresh.status != 0 ? refresh.status : -EIO;
            setPtraceFailure(ContextStatusError(status, true), status);
            return false;
        }
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
        if (!candidate.IsUsable()) {
            setPtraceFailure(
                CoordinateDecryptError::ContextDataInvalid, -ENODATA);
            return false;
        }
        context = candidate;
        executionContextSource =
            ProcessExecutionContextSource::PtraceOracle;
        executionContextDiagnostic.source =
            ProcessExecutionContextSource::PtraceOracle;
        executionContextDiagnostic.error = CoordinateDecryptError::None;
        executionContextDiagnostic.systemError = 0;
        return true;
    }

    ProcessExecutionContextDiagnostic ExecutionContextDiagnostic()
        const noexcept {
        std::lock_guard<std::mutex> lock(ioMutex);
        return executionContextDiagnostic;
    }

    bool RejectProcessExecutionContext() noexcept {
        std::lock_guard<std::mutex> lock(ioMutex);
        if (!open) return false;
        if (executionContextSource == ProcessExecutionContextSource::Device &&
            threadContextProvider != nullptr) {
            executionContextSource = ProcessExecutionContextSource::None;
            return threadContextProvider->RejectCurrent();
        }
        if (executionContextSource ==
                ProcessExecutionContextSource::PtraceOracle &&
            ptraceContextProvider != nullptr) {
            executionContextSource = ProcessExecutionContextSource::None;
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
                           RuntimeDiagnostic& diagnostic,
                           std::string& error) {
    if (impl_ == nullptr) {
        diagnostic = {
            RuntimeError::BackendUnavailable,
            -ENODEV,
        };
        error.clear();
        return false;
    }
    return impl_->Open(modeIndex, processId, processName, diagnostic, error);
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

bool MemoryTransport::ReadCoordinateMemory(
    std::uintptr_t address,
    void* destination,
    std::size_t size,
    CoordinateReadDiagnostic& diagnostic) {
    if (impl_ == nullptr) {
        diagnostic = {};
        diagnostic.address = address;
        diagnostic.size = size;
        diagnostic.failure =
            CoordinateReadFailure::TransportUnavailable;
        diagnostic.systemError = -ENODEV;
        return false;
    }
    return impl_->ReadCoordinateMemory(
        address, destination, size, diagnostic);
}

bool MemoryTransport::ReadCoordinateMemoryBatch(
    const MemoryReadRequest* requests,
    std::size_t count,
    CoordinateReadDiagnostic& diagnostic,
    std::size_t& failedIndex) {
    if (impl_ == nullptr) {
        diagnostic = {};
        failedIndex = 0;
        if (requests != nullptr && count != 0) {
            diagnostic.address = requests[0].remoteAddress;
            diagnostic.size = requests[0].size;
        }
        diagnostic.failure = CoordinateReadFailure::TransportUnavailable;
        diagnostic.systemError = -ENODEV;
        return false;
    }
    return impl_->ReadCoordinateMemoryBatch(
        requests, count, diagnostic, failedIndex);
}

std::size_t MemoryTransport::ReadBatch(
    const MemoryReadRequest* requests,
    std::size_t count,
    std::uint8_t* itemStatus) {
    return impl_ != nullptr
        ? impl_->ReadBatch(requests, count, itemStatus)
        : 0;
}

#if LENGJING_ENABLE_PROJECTILE_TRACKING
bool MemoryTransport::Write(
    std::uintptr_t address,
    const void* source,
    std::size_t size) {
    return impl_ != nullptr && impl_->Write(address, source, size);
}
#endif

std::uintptr_t MemoryTransport::ModuleBase(std::string_view moduleName) {
    return impl_ != nullptr ? impl_->ModuleBase(moduleName) : 0;
}

bool MemoryTransport::IsOpen() const noexcept {
    return impl_ != nullptr && impl_->IsOpen();
}

#if 0
bool MemoryTransport::SupportsExecutionBreakpoints() const noexcept {
    return impl_ != nullptr && impl_->SupportsExecutionBreakpoints();
}

bool MemoryTransport::ConfigureExecutionBreakpoint(
    std::uintptr_t address) noexcept {
    return impl_ != nullptr && impl_->ConfigureExecutionBreakpoint(address);
}

bool MemoryTransport::ReadExecutionBreakpointRecords(
    ExecutionBreakpointRecord* records,
    std::size_t capacity,
    std::size_t& recordsRead,
    std::uintptr_t& hitAddress,
    std::size_t& totalRecords) noexcept {
    if (impl_ == nullptr) {
        recordsRead = 0;
        hitAddress = 0;
        totalRecords = 0;
        return false;
    }
    return impl_->ReadExecutionBreakpointRecords(
        records, capacity, recordsRead, hitAddress, totalRecords);
}

bool MemoryTransport::RemoveExecutionBreakpoints() noexcept {
    return impl_ != nullptr && impl_->RemoveExecutionBreakpoints();
}
#endif

#if LENGJING_ENABLE_PROJECTILE_TRACKING
bool MemoryTransport::CanWrite() const noexcept {
    return impl_ != nullptr && impl_->CanWrite();
}

bool MemoryTransport::UsesKernelBackend() const noexcept {
    return impl_ != nullptr && impl_->UsesKernelBackend();
}
#endif

bool MemoryTransport::ConfigureCoordinateReplay(
    const CoordinateReplayTransportLayout& layout) noexcept {
    try {
        return impl_ != nullptr && impl_->ConfigureCoordinateReplay(layout);
    } catch (...) {
        return false;
    }
}

bool MemoryTransport::ResolveCoordinateReplayEntry(
    std::uintptr_t moduleBase,
    CoordinateReplayEntrySnapshot& snapshot,
    CoordinateReplayEntryDiagnostic& diagnostic) {
    try {
        return impl_ != nullptr && impl_->ResolveCoordinateReplayEntry(
            moduleBase, snapshot, diagnostic);
    } catch (...) {
        snapshot = {};
        diagnostic = {};
        diagnostic.error = CoordinateDecryptError::UnhandledException;
        diagnostic.systemError = -EFAULT;
        return false;
    }
}

bool MemoryTransport::ReadProcessExecutionContext(
    ProcessExecutionContext& context) {
    context = {};
    return impl_ != nullptr && impl_->ReadProcessExecutionContext(context);
}

ProcessExecutionContextDiagnostic
MemoryTransport::ExecutionContextDiagnostic() const noexcept {
    return impl_ != nullptr
        ? impl_->ExecutionContextDiagnostic()
        : ProcessExecutionContextDiagnostic{
            ProcessExecutionContextSource::None,
            CoordinateDecryptError::MemoryTransportUnavailable,
            -ENODEV,
        };
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
