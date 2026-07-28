#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <sys/types.h>

namespace lengjing::game::native {

struct SecureExecutionCapturePlan {
    std::uintptr_t entryPc = 0;
    std::uintptr_t callPc = 0;
    std::uintptr_t postLoadPc = 0;
    std::uint32_t storedRegister = 31;
    std::uint32_t loadedRegister = 31;
    std::uint32_t fallbackFpsr = 0;
    std::uint32_t fallbackFpcr = 0;
    bool metadataValid = false;
};

struct SecureExecutionCaptureResult {
    std::uint32_t fpsr = 0;
    std::uint32_t fpcr = 0;
    std::uint32_t storedValue = 0;
    std::uint64_t loadedValue = 0;
    bool entryCaptured = false;
    bool metadataCaptured = false;
};

class SecureKernelClient final {
public:
    SecureKernelClient() = default;
    ~SecureKernelClient();

    SecureKernelClient(const SecureKernelClient&) = delete;
    SecureKernelClient& operator=(const SecureKernelClient&) = delete;

    bool Read(pid_t processId,
              std::uintptr_t address,
              void* destination,
              std::size_t size,
              int& status) noexcept;
    std::uintptr_t ModuleBase(pid_t processId,
                              std::string_view moduleName,
                              std::size_t& moduleSize,
                              int& status) noexcept;
    bool ExecuteComputation(pid_t processId,
                            std::uint64_t data,
                            std::uint64_t modifier,
                            std::uint64_t& result,
                            int& status) noexcept;
    bool QueryNamedThreadTls(pid_t processId,
                             std::string_view threadName,
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

    void Close() noexcept;

private:
    bool EnsureConnected() noexcept;

    int descriptor_ = -1;
    bool installAttempted_ = false;
};

}  // namespace lengjing::game::native
