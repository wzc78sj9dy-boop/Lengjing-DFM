#pragma once

#include "game/native/ThreadExecutionContextProvider.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lengjing::game::native {

struct PacgaOracleInstruction {
    std::uintptr_t address = 0;
    std::uint64_t data = 0;
    std::uint64_t modifier = 0;

    constexpr bool IsValid() const noexcept {
        return address != 0 && (address & 3U) == 0;
    }

    constexpr bool operator==(
        const PacgaOracleInstruction& other) const noexcept {
        return address == other.address && data == other.data &&
            modifier == other.modifier;
    }
};

class PacgaOracleReader {
public:
    virtual ~PacgaOracleReader() = default;

    virtual int Read(std::int32_t threadId,
                     const PacgaOracleInstruction& instruction,
                     std::uint64_t& tpidrEl0,
                     std::uint64_t& result) = 0;
};

class PtracePacgaOracleReader final : public PacgaOracleReader {
public:
    PtracePacgaOracleReader();
    ~PtracePacgaOracleReader() override;

    PtracePacgaOracleReader(const PtracePacgaOracleReader&) = delete;
    PtracePacgaOracleReader& operator=(
        const PtracePacgaOracleReader&) = delete;

    int Read(std::int32_t threadId,
             const PacgaOracleInstruction& instruction,
             std::uint64_t& tpidrEl0,
             std::uint64_t& result) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct PtraceExecutionContextSnapshot {
    std::int32_t processId = 0;
    std::int32_t threadId = 0;
    std::uint64_t threadStartTimeTicks = 0;
    std::uint64_t tpidrEl0 = 0;
    PacgaOracleInstruction instruction{};
    std::uint64_t result = 0;
    std::uint64_t generation = 0;

    constexpr bool IsUsable() const noexcept {
        return processId > 0 && threadId > 0 && tpidrEl0 != 0 &&
            instruction.IsValid();
    }
};

struct PtraceExecutionContextRefresh {
    int status = 0;
    PtraceExecutionContextSnapshot snapshot{};

    constexpr bool HasContext() const noexcept {
        return status == 0 && snapshot.IsUsable();
    }
};

class PtraceExecutionContextProvider final {
public:
    PtraceExecutionContextProvider(
        std::int32_t processId,
        PacgaOracleReader& reader,
        std::string threadName = "GameThread",
        std::string procRoot = "/proc");

    PtraceExecutionContextRefresh Refresh(
        const PacgaOracleInstruction& instruction);
    bool RejectCurrent() noexcept;
    void Reset() noexcept;

private:
    PtraceExecutionContextRefresh InvalidateLocked(int status);

    std::int32_t processId_;
    PacgaOracleReader& reader_;
    std::string threadName_;
    ProcTaskThreadLocator locator_;
    mutable std::mutex mutex_;
    PtraceExecutionContextSnapshot current_{};
    std::vector<TaskThreadIdentity> rejected_;
    std::uint64_t generation_ = 0;
    std::chrono::steady_clock::time_point refreshAfter_{};
    std::chrono::steady_clock::time_point retryAfter_{};
    int lastStatus_ = 0;
};

}  // namespace lengjing::game::native
