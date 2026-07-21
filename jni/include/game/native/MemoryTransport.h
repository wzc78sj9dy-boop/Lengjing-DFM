#pragma once

#include "game/CoordinateDecryptDiagnostics.h"
#include "game/ProjectileTrackingFeature.h"
#include "game/RuntimeDiagnostics.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <sys/types.h>

namespace lengjing::game::native {

enum class MemoryTransportMode : int {
    ProcessVm = 0,
    KernelDriver = 1,
    PrivateRpc = 2,
    Count,
};

inline constexpr int kMemoryTransportModeCount =
    static_cast<int>(MemoryTransportMode::Count);

constexpr bool IsValidMemoryTransportMode(int mode) noexcept {
    return mode >= 0 && mode < kMemoryTransportModeCount;
}

constexpr bool IsKernelMemoryTransportMode(
    MemoryTransportMode mode) noexcept {
    return mode == MemoryTransportMode::KernelDriver ||
        mode == MemoryTransportMode::PrivateRpc;
}

struct CoordinateReplayTransportLayout {
    std::uintptr_t rootRva = 0x0E738950;
    std::uintptr_t bridgeOffset = 12;
    std::uintptr_t entryOffset = 0xA0;
    std::uint64_t pacgaData = UINT64_C(0x412625C7);
    std::uint64_t pacgaModifier = UINT64_C(0xBB7AC00B);

    constexpr bool IsValid() const noexcept {
        return rootRva >= 4 && rootRva <= 0xffffffffULL &&
            (rootRva & 3U) == 0 &&
            bridgeOffset <= 0x10000 && (bridgeOffset & 3U) == 0 &&
            bridgeOffset <= 0xffffffffULL - rootRva &&
            entryOffset >= 8 && entryOffset <= 0x10000 &&
            (entryOffset & 7U) == 0 &&
            (pacgaData != 0 || pacgaModifier != 0);
    }
};

struct ProcessExecutionContext {
    struct PacgaOracle {
        std::uint64_t data = 0;
        std::uint64_t modifier = 0;
        std::uint64_t result = 0;
        bool available = false;

        constexpr bool Matches(std::uint64_t candidateData,
                               std::uint64_t candidateModifier) const
            noexcept {
            return available && data == candidateData &&
                modifier == candidateModifier;
        }
    };

    std::uint64_t tpidrEl0 = 0;
    std::uint64_t pacgaLow = 0;
    std::uint64_t pacgaHigh = 0;
    std::int32_t threadId = 0;
    std::uint64_t threadStartTimeTicks = 0;
    std::uint64_t generation = 0;
    PacgaOracle pacgaOracle{};

    constexpr bool HasPacgaKey() const noexcept {
        return pacgaLow != 0 || pacgaHigh != 0;
    }

    constexpr bool IsUsable() const noexcept {
        return threadId > 0 && tpidrEl0 != 0 &&
            (HasPacgaKey() || pacgaOracle.available);
    }
};

enum class ProcessExecutionContextSource : std::uint8_t {
    None,
    Device,
    PtraceOracle,
};

struct ProcessExecutionContextDiagnostic {
    ProcessExecutionContextSource source =
        ProcessExecutionContextSource::None;
    CoordinateDecryptError error = CoordinateDecryptError::None;
    int systemError = 0;
    int deviceStatus = 0;
    int ptraceStatus = 0;
    std::size_t deviceRequestCount = 0;
    bool pacgaOperandsResolved = false;
};

struct MemoryReadRequest {
    std::uintptr_t remoteAddress = 0;
    void* localBuffer = nullptr;
    std::size_t size = 0;
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
    void Close() noexcept;

    bool Read(std::uintptr_t address, void* destination, std::size_t size);
    bool ReadCoordinateMemory(
        std::uintptr_t address,
        void* destination,
        std::size_t size,
        CoordinateReadDiagnostic& diagnostic);
    std::size_t ReadBatch(const MemoryReadRequest* requests,
                          std::size_t count,
                          std::uint8_t* itemStatus = nullptr);
#if LENGJING_ENABLE_PROJECTILE_TRACKING
    bool Write(std::uintptr_t address, const void* source, std::size_t size);
#endif
    std::uintptr_t ModuleBase(std::string_view moduleName);
    bool IsOpen() const noexcept;
#if LENGJING_ENABLE_PROJECTILE_TRACKING
    bool CanWrite() const noexcept;
    bool UsesKernelBackend() const noexcept;
#endif
    bool ConfigureCoordinateReplay(
        const CoordinateReplayTransportLayout& layout) noexcept;
    bool ReadProcessExecutionContext(ProcessExecutionContext& context);
    ProcessExecutionContextDiagnostic ExecutionContextDiagnostic()
        const noexcept;
    bool RejectProcessExecutionContext() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

pid_t FindProcessId(std::string_view processName);
bool IsProcessAlive(pid_t processId);
std::uintptr_t FindMappedModuleBase(pid_t processId, std::string_view moduleName);

}  // namespace lengjing::game::native
