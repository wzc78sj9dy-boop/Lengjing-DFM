#pragma once

#include "game/aim/TrackingCalculator.h"

#include <chrono>
#include <cstdint>

#include <sys/types.h>

namespace lengjing::game::native {

class MemoryTransport;

class TrajectoryHook final {
public:
    TrajectoryHook() = default;
    ~TrajectoryHook();

    TrajectoryHook(const TrajectoryHook&) = delete;
    TrajectoryHook& operator=(const TrajectoryHook&) = delete;

    bool EnsureInstalled(MemoryTransport& memory,
                         pid_t processId,
                         std::uintptr_t moduleBase) noexcept;
    bool Publish(const aim::TrackingCommand& command) noexcept;
    bool Disable() noexcept;
    bool Shutdown() noexcept;
    bool Installed() const noexcept;

private:
    enum class InstallResult : std::uint8_t {
        Installed,
        RetryableFailure,
        PermanentFailure,
    };

    InstallResult Install() noexcept;
    bool ClearCommandBuffer() noexcept;
    void ResetLocal() noexcept;

    MemoryTransport* memory_ = nullptr;
    pid_t processId_ = -1;
    std::uintptr_t moduleBase_ = 0;
    std::uintptr_t commandAddress_ = 0;
    std::uintptr_t firstStubAddress_ = 0;
    std::uintptr_t secondStubAddress_ = 0;
    std::uintptr_t firstPatchAddress_ = 0;
    std::uintptr_t secondPatchAddress_ = 0;
    std::chrono::steady_clock::time_point retryAfter_{};
    std::uint8_t installAttempts_ = 0;
    bool permanentInstallFailure_ = false;
    bool cleanupRequired_ = false;
    bool installed_ = false;
};

}  // namespace lengjing::game::native
