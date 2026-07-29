#include "game/native/MemoryTransport.h"

#include "game/native/Arm64Pacga.h"
#include "game/native/KernelModuleLoader.h"
#include "game/native/PerfExecutionBreakpoint.h"
#include "game/native/PtraceExecutionContextProvider.h"
#include "game/native/ThreadContextDeviceTransport.h"
#include "game/native/ThreadExecutionContextProvider.h"
#include "platform/PerformanceTrace.h"
#include "paradise/paradise_api.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>

namespace lengjing::game::native {
namespace {

static_assert(sizeof(hwbp_point_config) == 24,
              "unexpected Paradise breakpoint point layout");
static_assert(sizeof(hwbp_record) == 848,
              "unexpected Paradise breakpoint record layout");
static_assert(HWBP_MAX_RECORDS == kExecutionBreakpointRecordLimit,
              "Paradise breakpoint record limit mismatch");

constexpr std::uintptr_t kMinimumRemoteAddress = 0x10000000ULL;
constexpr std::uintptr_t kMaximumRemoteAddress = 0x10000000000ULL;
constexpr char kDefaultThreadContextDevice[] = "/dev/fbe775";
constexpr unsigned int kExecutionMapSeedPollCount = 25;

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

bool ReadBreakpointRegister(const hwbp_record& record,
                            std::uint32_t index,
                            std::uint64_t& value) noexcept {
    value = 0;
    if (index <= 29) {
        static_assert(
            offsetof(hwbp_record, x29) - offsetof(hwbp_record, x0) ==
                29 * sizeof(std::uint64_t));
        const auto* source =
            reinterpret_cast<const std::uint8_t*>(&record) +
            offsetof(hwbp_record, x0) +
            index * sizeof(std::uint64_t);
        std::memcpy(&value, source, sizeof(value));
        return true;
    }
    if (index == 30) {
        value = record.lr;
        return true;
    }
    return false;
}

bool IsStableBreakpointRecord(const hwbp_record& previous,
                              const hwbp_record& current,
                              std::uint32_t registerIndex) noexcept {
    std::uint64_t previousValue = 0;
    std::uint64_t currentValue = 0;
    return previous.tid == current.tid &&
        previous.hit_count == current.hit_count &&
        previous.pc == current.pc &&
        previous.sp == current.sp &&
        ReadBreakpointRegister(
            previous, registerIndex, previousValue) &&
        ReadBreakpointRegister(
            current, registerIndex, currentValue) &&
        previousValue == currentValue;
}

struct MemoryTransferResult {
    int status = 0;
    std::size_t completed = 0;
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

std::string MutableName(std::string_view value) {
    return std::string(value.begin(), value.end());
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

}  // namespace

struct MemoryTransport::Impl {
    enum class ExecutionBreakpointBackend : std::uint8_t {
        None,
        Kernel,
        Perf,
    };

    MemoryTransportMode mode = MemoryTransportMode::ProcessVm;
    pid_t processId = -1;
    paradise_driver* kernel = nullptr;
    int threadContextFd = -1;
    std::unique_ptr<ThreadContextDeviceTransport> threadContextTransport;
    std::unique_ptr<ThreadExecutionContextProvider> threadContextProvider;
    PtracePacgaOracleReader ptraceOracleReader;
    ProcTaskThreadLocator threadLocator;
    pid_t contextThreadId = -1;
    std::uintptr_t contextThreadPointer = 0;
    Arm64PacgaKey contextPacgaKey{};
    bool contextPacgaKeyAvailable = false;
    std::string contextThreadName;
    PerfExecutionBreakpoint perfExecutionBreakpoint;
    ExecutionBreakpointBackend executionBreakpointBackend =
        ExecutionBreakpointBackend::None;
    bool executionBreakpointConfigured = false;
    std::uintptr_t executionBreakpointAddress = 0;
    std::array<hwbp_record, HWBP_MAX_RECORDS>
        executionBreakpointRecordBuffer{};
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
        static_cast<void>(RemoveExecutionBreakpointsUnlocked());
        threadContextProvider.reset();
        threadContextTransport.reset();
        if (threadContextFd >= 0) {
            ::close(threadContextFd);
            threadContextFd = -1;
        }
        contextThreadId = -1;
        contextThreadPointer = 0;
        contextPacgaKey = {};
        contextPacgaKeyAvailable = false;
        contextThreadName.clear();
        delete kernel;
        kernel = nullptr;
        mode = MemoryTransportMode::ProcessVm;
        processId = -1;
        open = false;
#if LENGJING_ENABLE_PROJECTILE_TRACKING
        writable = false;
#endif
    }

    void Close() noexcept {
        std::lock_guard<std::mutex> lock(ioMutex);
        ResetUnlocked();
    }

    bool Open(int modeIndex,
              pid_t targetProcessId,
              std::string_view processName,
              RuntimeDiagnostic& diagnostic,
              std::string& error,
              bool allowWrite) {
        static_cast<void>(processName);
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

#if LENGJING_ENABLE_PROJECTILE_TRACKING
        if (!allowWrite) writable = false;
#else
        static_cast<void>(allowWrite);
#endif
        open = true;
        diagnostic = {};
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
                processId, address, destination, size, false).status == 0;
        }
        if (mode == MemoryTransportMode::KernelDriver) {
            return kernel != nullptr &&
                kernel->read_fast(address, destination, size);
        }
        return false;
    }

    bool ReadWithKind(std::uintptr_t address,
                      void* destination,
                      std::size_t size,
                      platform::PerformancePhase phase,
                      platform::PerformanceReadKind kind) {
        if (!platform::PerformanceTraceEnabled()) {
            std::lock_guard<std::mutex> lock(ioMutex);
            return ReadUnlocked(address, destination, size);
        }
        platform::PerformanceTraceScope trace(
            phase, 64);
        std::lock_guard<std::mutex> lock(ioMutex);
        const bool read = ReadUnlocked(address, destination, size);
        platform::RecordPerformanceRead(
            kind,
            size,
            read);
        return read;
    }

    bool Read(std::uintptr_t address, void* destination, std::size_t size) {
        return ReadWithKind(
            address,
            destination,
            size,
            platform::PerformancePhase::RemoteRead,
            platform::PerformanceReadKind::Standard);
    }

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

    bool CaptureExecutionMapSeed(
        pid_t threadId,
        const ExecutionMapSeedPlan& plan,
        ExecutionMapSeed& seed,
        int& status) noexcept {
        seed = {};
        status = -EINVAL;
        std::lock_guard<std::mutex> lock(ioMutex);
        if (!open || processId <= 0 || threadId <= 0 ||
            !plan.IsValid() ||
            !IsRemoteRangeValid(plan.callPc, sizeof(std::uint32_t)) ||
            !IsRemoteRangeValid(
                plan.postLoadPc, sizeof(std::uint32_t))) {
            return false;
        }
        if (mode != MemoryTransportMode::KernelDriver ||
            kernel == nullptr) {
            status = -EOPNOTSUPP;
            return false;
        }
        if (executionBreakpointConfigured) {
            status = -EBUSY;
            return false;
        }

        std::uint64_t breakpointCount = 0;
        std::uint64_t watchpointCount = 0;
        bool setAttempted = false;
        const auto removeBreakpoints = [&]() noexcept {
            if (!setAttempted) return true;
            setAttempted = false;
            try {
                return kernel->hwbp_remove(processId);
            } catch (...) {
                return false;
            }
        };
        try {
            if (!kernel->hwbp_get_info(
                    &breakpointCount, &watchpointCount) ||
                breakpointCount < 2) {
                status = -ENOSPC;
                return false;
            }

            std::array<hwbp_point_config, 2> points{{
                {
                    HWBP_BREAKPOINT_X,
                    HWBP_BREAKPOINT_LEN_4,
                    SCOPE_ALL_THREADS,
                    static_cast<std::uint64_t>(plan.callPc),
                },
                {
                    HWBP_BREAKPOINT_X,
                    HWBP_BREAKPOINT_LEN_4,
                    SCOPE_ALL_THREADS,
                    static_cast<std::uint64_t>(plan.postLoadPc),
                },
            }};
            setAttempted = true;
            if (!kernel->hwbp_set(
                    processId,
                    points.data(),
                    static_cast<int>(points.size()))) {
                status = -EIO;
                static_cast<void>(removeBreakpoints());
                return false;
            }

            std::array<hwbp_record, 2> previousRecords{};
            std::array<hwbp_record, 2> stableRecords{};
            std::array<bool, 2> previousSeen{};
            for (unsigned int poll = 0;
                 poll < kExecutionMapSeedPollCount;
                 ++poll) {
                std::array<bool, 2> stableSeen{};
                for (std::size_t point = 0;
                     point < points.size();
                     ++point) {
                    std::uint64_t hitAddress = 0;
                    int totalRecords = 0;
                    const int copied = kernel->hwbp_read_records(
                        processId,
                        static_cast<int>(point),
                        executionBreakpointRecordBuffer.data(),
                        static_cast<int>(
                            executionBreakpointRecordBuffer.size()),
                        &hitAddress,
                        &totalRecords);
                    if (copied < 0 ||
                        static_cast<std::size_t>(copied) >
                            executionBreakpointRecordBuffer.size() ||
                        totalRecords < 0 ||
                        totalRecords > HWBP_MAX_RECORDS) {
                        status = -EIO;
                        static_cast<void>(removeBreakpoints());
                        return false;
                    }
                    if (hitAddress != 0 &&
                        hitAddress != points[point].hit_addr) {
                        continue;
                    }
                    const hwbp_record* latest = nullptr;
                    for (int index = 0; index < copied; ++index) {
                        const hwbp_record& candidate =
                            executionBreakpointRecordBuffer[
                                static_cast<std::size_t>(index)];
                        if (candidate.tid != threadId ||
                            candidate.hit_count == 0 ||
                            candidate.pc != points[point].hit_addr ||
                            (latest != nullptr &&
                             candidate.hit_count <= latest->hit_count)) {
                            continue;
                        }
                        latest = &candidate;
                    }
                    if (latest == nullptr) continue;
                    const std::uint32_t registerIndex =
                        point == 0
                            ? plan.storedRegister
                            : plan.loadedRegister;
                    if (previousSeen[point] &&
                        IsStableBreakpointRecord(
                            previousRecords[point],
                            *latest,
                            registerIndex)) {
                        stableRecords[point] = *latest;
                        stableSeen[point] = true;
                    }
                    previousRecords[point] = *latest;
                    previousSeen[point] = true;
                }
                if (stableSeen[0] && stableSeen[1]) {
                    std::uint64_t stored = 0;
                    std::uint64_t loaded = 0;
                    if (!ReadBreakpointRegister(
                            stableRecords[0],
                            plan.storedRegister,
                            stored) ||
                        !ReadBreakpointRegister(
                            stableRecords[1],
                            plan.loadedRegister,
                            loaded)) {
                        status = -EIO;
                        static_cast<void>(removeBreakpoints());
                        return false;
                    }
                    seed = {
                        static_cast<std::uint32_t>(stored),
                        loaded,
                    };
                    if (seed.IsValid()) break;
                }
                if (poll + 1 < kExecutionMapSeedPollCount) {
                    usleep(1000);
                }
            }

            if (!removeBreakpoints()) {
                status = -EIO;
                return false;
            }
            if (!seed.IsValid()) {
                status = -ETIMEDOUT;
                return false;
            }
            status = 0;
            return true;
        } catch (...) {
            static_cast<void>(removeBreakpoints());
            status = -EIO;
            return false;
        }
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
                    static_cast<std::uintptr_t>(source.x20),
                    static_cast<std::uintptr_t>(source.x21),
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

    bool ReadGeometry(std::uintptr_t address,
                      void* destination,
                      std::size_t size) {
        return ReadWithKind(
            address,
            destination,
            size,
            platform::PerformancePhase::GeometryRemoteRead,
            platform::PerformanceReadKind::Geometry);
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
        const auto finish = [&](std::size_t successful) {
            if (traceEnabled) {
                platform::RecordPerformanceRead(
                    platform::PerformanceReadKind::Batch,
                    requestedBytes,
                    successful == count,
                    count);
            }
            return successful;
        };
        std::unique_lock<std::mutex> lock(ioMutex);
        if (count == 0) return finish(0);
        if (itemStatus != nullptr) {
            std::fill_n(itemStatus, count, static_cast<std::uint8_t>(0));
        }
        if (!open || requests == nullptr) return finish(0);

        for (std::size_t index = 0; index < count; ++index) {
            if (requests[index].localBuffer == nullptr ||
                !IsRemoteRangeValid(
                    requests[index].remoteAddress, requests[index].size)) {
                return finish(0);
            }
        }

        const std::uint64_t batchGeneration = ioGeneration;
        platform::RecordPerformanceCount(
            mode == MemoryTransportMode::ProcessVm
                ? platform::PerformanceCounter::BatchProcessVmCalls
                : platform::PerformanceCounter::BatchKernelDriverCalls);
        platform::RecordPerformanceCount(
            platform::PerformanceCounter::BatchFallbackItems, count);
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
        return finish(successful);
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
        return false;
    }
#endif

    std::uintptr_t ModuleBase(std::string_view moduleName) {
        std::lock_guard<std::mutex> lock(ioMutex);
        if (!open || processId < 1 || moduleName.empty()) return 0;
        if (mode == MemoryTransportMode::KernelDriver &&
            kernel != nullptr) {
            const std::string name = MutableName(moduleName);
            try {
                const std::uintptr_t base =
                    kernel->get_module_base(name.c_str());
                if (base != 0) return base;
            } catch (...) {
            }
        }

        MappedModuleRange range{};
        return FindMappedModuleRange(processId, moduleName, range)
            ? range.begin
            : 0;
    }

    bool OpenThreadContextDeviceUnlocked(std::string_view threadName) {
        if (threadContextProvider != nullptr &&
            contextThreadName == threadName) {
            return true;
        }

        threadContextProvider.reset();
        threadContextTransport.reset();
        if (threadContextFd >= 0) {
            ::close(threadContextFd);
            threadContextFd = -1;
        }

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
        threadContextProvider =
            std::make_unique<ThreadExecutionContextProvider>(
                processId,
                *threadContextTransport,
                MutableName(threadName));
        contextThreadName = MutableName(threadName);
        return true;
    }

    bool ExecutePacga(std::uintptr_t instructionAddress,
                      std::uint32_t instruction,
                      std::uint64_t data,
                      std::uint64_t modifier,
                      std::uint64_t& result,
                      int& status) noexcept {
        result = 0;
        status = -EINVAL;
        std::lock_guard<std::mutex> lock(ioMutex);
        if (!open || processId < 1 ||
            instructionAddress == 0 ||
            (instructionAddress & 3U) != 0 ||
            (instruction & UINT32_C(0xFFE0FC00)) !=
                UINT32_C(0x9AC03000)) {
            return false;
        }

        if (contextPacgaKeyAvailable) {
            result = ComputeArm64Pacga(
                data, modifier, contextPacgaKey);
            status = 0;
            return true;
        }
        if (contextThreadId < 1) {
            status = -ENODATA;
            return false;
        }

        std::uint64_t fingerprint =
            static_cast<std::uint64_t>(instructionAddress) ^
            (static_cast<std::uint64_t>(instruction) << 32U) ^
            UINT64_C(0xCBF29CE484222325);
        if (fingerprint == 0) fingerprint = 1;
        const PacgaOracleInstruction oracle{
            instructionAddress,
            data,
            modifier,
            instruction,
            {instructionAddress, fingerprint},
        };
        std::uint64_t threadPointer = 0;
        status = ptraceOracleReader.Read(
            contextThreadId, oracle, threadPointer, result);
        if (status == 0) {
            contextThreadPointer =
                static_cast<std::uintptr_t>(threadPointer);
            return true;
        }

        std::vector<TaskThreadIdentity> identities;
        if (contextThreadName.empty() ||
            threadLocator.FindAllExact(
                processId, contextThreadName, identities) != 0) {
            return false;
        }
        for (const TaskThreadIdentity& identity : identities) {
            if (identity.threadId == contextThreadId) continue;
            threadPointer = 0;
            const int candidateStatus = ptraceOracleReader.Read(
                identity.threadId, oracle, threadPointer, result);
            if (candidateStatus != 0) {
                status = candidateStatus;
                continue;
            }
            contextThreadId = identity.threadId;
            contextThreadPointer =
                static_cast<std::uintptr_t>(threadPointer);
            status = 0;
            return true;
        }
        return false;
    }

    bool QueryNamedThreadTls(std::string_view name,
                             pid_t& threadId,
                             std::uintptr_t& tls,
                             int& status) noexcept {
        threadId = -1;
        tls = 0;
        status = -EINVAL;
        std::lock_guard<std::mutex> lock(ioMutex);
        if (!open || processId < 1) return false;

        if (OpenThreadContextDeviceUnlocked(name) &&
            threadContextProvider != nullptr) {
            const ThreadExecutionContextRefresh refresh =
                threadContextProvider->Refresh();
            if (refresh.HasThreadContext()) {
                threadId = refresh.snapshot.threadId;
                tls = static_cast<std::uintptr_t>(
                    refresh.snapshot.tpidrEl0);
                contextThreadId = threadId;
                contextThreadPointer = tls;
                contextThreadName = MutableName(name);
                contextPacgaKey = {
                    refresh.snapshot.apga.low,
                    refresh.snapshot.apga.high,
                };
                contextPacgaKeyAvailable =
                    refresh.snapshot.HasPacgaKey();
                status = 0;
                return true;
            }
            status = refresh.status != 0
                ? refresh.status
                : refresh.pacgaStatus;
        } else {
            status = errno != 0 ? -errno : -ENODEV;
        }

        std::vector<TaskThreadIdentity> identities;
        const int locateStatus =
            threadLocator.FindAllExact(processId, name, identities);
        if (locateStatus != 0) {
            status = locateStatus;
            return false;
        }
        for (const TaskThreadIdentity& identity : identities) {
            std::uint64_t threadPointer = 0;
            const int candidateStatus =
                ptraceOracleReader.ReadThreadPointer(
                    identity.threadId, threadPointer);
            if (candidateStatus != 0) {
                status = candidateStatus;
                continue;
            }
            threadId = identity.threadId;
            tls = static_cast<std::uintptr_t>(threadPointer);
            contextThreadId = threadId;
            contextThreadPointer = tls;
            contextThreadName = MutableName(name);
            contextPacgaKey = {};
            contextPacgaKeyAvailable = false;
            status = 0;
            return true;
        }
        return false;
    }

    bool QueryThreadStack(pid_t threadId,
                          std::uintptr_t expectedTls,
                          std::uintptr_t& low,
                          std::uintptr_t& high,
                          int& status) noexcept {
        low = 0;
        high = 0;
        status = -EINVAL;
        std::lock_guard<std::mutex> lock(ioMutex);
        if (!open || processId < 1 || threadId < 1) return false;

        const std::string path =
            "/proc/" + std::to_string(processId) + "/maps";
        std::ifstream maps(path);
        if (!maps) {
            status = errno != 0 ? -errno : -EIO;
            return false;
        }
        const std::string marker =
            "[anon:stack_and_tls:" + std::to_string(threadId) + "]";
        std::string line;
        while (std::getline(maps, line)) {
            if (line.find(marker) == std::string::npos) continue;
            unsigned long long begin = 0;
            unsigned long long end = 0;
            if (std::sscanf(
                    line.c_str(), "%llx-%llx", &begin, &end) != 2 ||
                end <= begin ||
                expectedTls < begin ||
                expectedTls >= end) {
                continue;
            }
            low = static_cast<std::uintptr_t>(begin);
            high = static_cast<std::uintptr_t>(end);
            status = 0;
            return true;
        }
        status = -ENOENT;
        return false;
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
        return open && mode == MemoryTransportMode::KernelDriver &&
            kernel != nullptr;
    }
#endif

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
    return impl_->Open(
        modeIndex, processId, processName, diagnostic, error, true);
}

bool MemoryTransport::OpenReadOnly(int modeIndex,
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
    return impl_->Open(
        modeIndex, processId, processName, diagnostic, error, false);
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

bool MemoryTransport::ReadGeometry(
    std::uintptr_t address,
    void* destination,
    std::size_t size) {
    return impl_ != nullptr &&
        impl_->ReadGeometry(address, destination, size);
}

std::size_t MemoryTransport::ReadBatch(
    const MemoryReadRequest* requests,
    std::size_t count,
    std::uint8_t* itemStatus) {
    return impl_ != nullptr
        ? impl_->ReadBatch(requests, count, itemStatus)
        : 0;
}

bool MemoryTransport::ExecutePacga(std::uintptr_t instructionAddress,
                                   std::uint32_t instruction,
                                   std::uint64_t data,
                                   std::uint64_t modifier,
                                   std::uint64_t& result,
                                   int& status) noexcept {
    if (impl_ == nullptr) {
        result = 0;
        status = -EINVAL;
        return false;
    }
    return impl_->ExecutePacga(
        instructionAddress,
        instruction,
        data,
        modifier,
        result,
        status);
}

bool MemoryTransport::QueryThreadStack(
    pid_t threadId,
    std::uintptr_t expectedTls,
    std::uintptr_t& low,
    std::uintptr_t& high,
    int& status) noexcept {
    if (impl_ == nullptr) {
        low = 0;
        high = 0;
        status = -EINVAL;
        return false;
    }
    return impl_->QueryThreadStack(
        threadId,
        expectedTls,
        low,
        high,
        status);
}

bool MemoryTransport::QueryNamedThreadTls(
    std::string_view name,
    pid_t& threadId,
    std::uintptr_t& tls,
    int& status) noexcept {
    if (impl_ == nullptr) {
        threadId = -1;
        tls = 0;
        status = -EINVAL;
        return false;
    }
    return impl_->QueryNamedThreadTls(
        name, threadId, tls, status);
}

bool MemoryTransport::CaptureExecutionMapSeed(
    pid_t threadId,
    const ExecutionMapSeedPlan& plan,
    ExecutionMapSeed& seed,
    int& status) noexcept {
    if (impl_ == nullptr) {
        seed = {};
        status = -EINVAL;
        return false;
    }
    return impl_->CaptureExecutionMapSeed(
        threadId, plan, seed, status);
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

#if LENGJING_ENABLE_PROJECTILE_TRACKING
bool MemoryTransport::CanWrite() const noexcept {
    return impl_ != nullptr && impl_->CanWrite();
}

bool MemoryTransport::UsesKernelBackend() const noexcept {
    return impl_ != nullptr && impl_->UsesKernelBackend();
}
#endif

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
    MappedModuleRange range{};
    return FindMappedModuleRange(processId, moduleName, range)
        ? range.begin
        : 0;
}

bool FindMappedModuleRange(pid_t processId,
                           std::string_view moduleName,
                           MappedModuleRange& range) {
    range = {};
    if (processId <= 0 || moduleName.empty()) return false;
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%d/maps", processId);
    std::ifstream input(path);
    if (!input) return false;

    std::string line;
    while (std::getline(input, line)) {
        if (line.find(moduleName) == std::string::npos) continue;
        const std::size_t separator = line.find('-');
        if (separator == std::string::npos) continue;
        const std::size_t addressEnd = line.find(' ', separator + 1);
        if (addressEnd == std::string::npos) continue;

        const std::string beginText = line.substr(0, separator);
        const std::string endText = line.substr(
            separator + 1, addressEnd - separator - 1);
        char* parsedBeginEnd = nullptr;
        char* parsedEndEnd = nullptr;
        const unsigned long long begin = std::strtoull(
            beginText.c_str(), &parsedBeginEnd, 16);
        const unsigned long long end = std::strtoull(
            endText.c_str(), &parsedEndEnd, 16);
        if (parsedBeginEnd == beginText.c_str() ||
            parsedEndEnd == endText.c_str() ||
            *parsedBeginEnd != '\0' || *parsedEndEnd != '\0' ||
            begin < kMinimumRemoteAddress ||
            begin >= kMaximumRemoteAddress ||
            end <= begin || end > kMaximumRemoteAddress) {
            continue;
        }
        const auto mappingBegin = static_cast<std::uintptr_t>(begin);
        const auto mappingEnd = static_cast<std::uintptr_t>(end);
        range.begin = range.begin == 0
            ? mappingBegin
            : std::min(range.begin, mappingBegin);
        range.end = std::max(range.end, mappingEnd);
    }
    return range.IsValid();
}

}  // namespace lengjing::game::native
