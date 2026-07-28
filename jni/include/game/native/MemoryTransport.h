#pragma once

#include "game/ProjectileTrackingFeature.h"
#include "game/RuntimeDiagnostics.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <sys/types.h>

namespace lengjing::game::native {

struct SecureExecutionCapturePlan;
struct SecureExecutionCaptureResult;

enum class MemoryTransportMode : int {
    ProcessVm = 0,
    KernelDriver = 1,
    Count,
};

inline constexpr int kMemoryTransportModeCount =
    static_cast<int>(MemoryTransportMode::Count);

constexpr bool IsValidMemoryTransportMode(int mode) noexcept {
    return mode >= 0 && mode < kMemoryTransportModeCount;
}

constexpr bool IsKernelMemoryTransportMode(
    MemoryTransportMode mode) noexcept {
    return mode == MemoryTransportMode::KernelDriver;
}

struct MemoryReadRequest {
    std::uintptr_t remoteAddress = 0;
    void* localBuffer = nullptr;
    std::size_t size = 0;
};

inline constexpr std::size_t kExecutionBreakpointRecordLimit = 0x100;

struct ExecutionBreakpointRecord {
    pid_t tid = -1;
    std::uint64_t hitCount = 0;
    std::uintptr_t pc = 0;
    std::uintptr_t sp = 0;
    std::uintptr_t x0 = 0;
    std::uintptr_t x20 = 0;
    std::uintptr_t x21 = 0;
    std::uintptr_t x23 = 0;
};

class MemoryTransport final {
public:
    MemoryTransport();
    ~MemoryTransport();

    MemoryTransport(const MemoryTransport&) = delete;
    MemoryTransport& operator=(const MemoryTransport&) = delete;

    bool Open(int modeIndex,
              pid_t processId,
              std::string_view processName,
              RuntimeDiagnostic& diagnostic,
              std::string& error);
    bool OpenReadOnly(int modeIndex,
                      pid_t processId,
                      std::string_view processName,
                      RuntimeDiagnostic& diagnostic,
                      std::string& error);
    void Close() noexcept;

    bool Read(std::uintptr_t address, void* destination, std::size_t size);
    bool ReadGeometry(std::uintptr_t address,
                      void* destination,
                      std::size_t size);
    std::size_t ReadBatch(const MemoryReadRequest* requests,
                          std::size_t count,
                          std::uint8_t* itemStatus = nullptr);
    bool ReadSecure(std::uintptr_t address,
                    void* destination,
                    std::size_t size,
                    int& status) noexcept;
    std::uintptr_t ModuleBaseSecure(
        std::string_view moduleName,
        std::size_t& moduleSize,
        int& status) noexcept;
    bool ExecutePacga(std::uint64_t data,
                      std::uint64_t modifier,
                      std::uint64_t& result,
                      int& status) noexcept;
    bool QueryNamedThreadTls(std::string_view name,
                             pid_t& threadId,
                             std::uintptr_t& tls,
                             int& status) noexcept;
    bool QueryThreadStack(pid_t threadId,
                          std::uintptr_t expectedTls,
                          std::uintptr_t& low,
                          std::uintptr_t& high,
                          int& status) noexcept;
    bool CaptureExecutionState(
        pid_t threadId,
        const SecureExecutionCapturePlan& plan,
        SecureExecutionCaptureResult& result,
        int& status) noexcept;
#if LENGJING_ENABLE_PROJECTILE_TRACKING
    bool Write(std::uintptr_t address, const void* source, std::size_t size);
#endif
    std::uintptr_t ModuleBase(std::string_view moduleName);
    bool IsOpen() const noexcept;
    bool SupportsExecutionBreakpoints() const noexcept;
    bool ConfigureExecutionBreakpoint(std::uintptr_t address) noexcept;
    bool ReadExecutionBreakpointRecords(
        ExecutionBreakpointRecord* records,
        std::size_t capacity,
        std::size_t& recordsRead,
        std::uintptr_t& hitAddress,
        std::size_t& totalRecords) noexcept;
    bool RemoveExecutionBreakpoints() noexcept;
#if LENGJING_ENABLE_PROJECTILE_TRACKING
    bool CanWrite() const noexcept;
    bool UsesKernelBackend() const noexcept;
#endif
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

pid_t FindProcessId(std::string_view processName);
bool IsProcessAlive(pid_t processId);

struct MappedModuleRange {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;

    constexpr bool IsValid() const noexcept {
        return begin != 0 && end > begin;
    }

    constexpr std::size_t Size() const noexcept {
        return IsValid() ? static_cast<std::size_t>(end - begin) : 0;
    }
};

bool FindMappedModuleRange(pid_t processId,
                           std::string_view moduleName,
                           MappedModuleRange& range);
std::uintptr_t FindMappedModuleBase(pid_t processId, std::string_view moduleName);

}  // namespace lengjing::game::native
