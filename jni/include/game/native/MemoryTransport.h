#pragma once

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
              std::string& error);
    void Close() noexcept;

    bool Read(std::uintptr_t address, void* destination, std::size_t size);
    std::size_t ReadBatch(const MemoryReadRequest* requests,
                          std::size_t count,
                          std::uint8_t* itemStatus = nullptr);
    bool Write(std::uintptr_t address, const void* source, std::size_t size);
    std::uintptr_t ModuleBase(std::string_view moduleName);
    bool IsOpen() const noexcept;
    bool CanWrite() const noexcept;
    bool UsesKernelBackend() const noexcept;
    bool ReadProcessExecutionContext(ProcessExecutionContext& context);
    bool RejectProcessExecutionContext() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

pid_t FindProcessId(std::string_view processName);
bool IsProcessAlive(pid_t processId);
std::uintptr_t FindMappedModuleBase(pid_t processId, std::string_view moduleName);

}  // namespace lengjing::game::native
