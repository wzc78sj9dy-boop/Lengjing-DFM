#pragma once

#include "game/CoordinateDecryptDiagnostics.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>
#include <system_error>

namespace lengjing::game::native {

class MemoryTransport;
struct ProcessExecutionContext;

struct AlgorithmPacgaKey {
    std::uint64_t low = 0;
    std::uint64_t high = 0;
};

struct AlgorithmPacgaOracle {
    std::uint64_t data = 0;
    std::uint64_t modifier = 0;
    std::uint64_t result = 0;
    bool available = false;

    constexpr bool Matches(std::uint64_t candidateData,
                           std::uint64_t candidateModifier) const noexcept {
        return available && data == candidateData &&
            modifier == candidateModifier;
    }
};

enum class AlgorithmPositionRuntimeStage : std::uint8_t {
    Idle,
    Queued,
    Preparing,
    RefreshingPages,
    Executing,
    ReadingResult,
    Completed,
    Failed,
};

enum class AlgorithmPositionRuntimeError : std::uint16_t {
    None = 0,
    InvalidInput,
    EngineSetupFailed,
    PageRefreshFailed,
    RegisterSetupFailed,
    EmulationFailed,
    MemoryHookFailed,
    Timeout,
    ReturnPcMismatch,
    ResultReadFailed,
    ResultInvalid,
    PacgaUnavailable,
    UnsupportedSvc,
    ContextStale,
    FaultAddressInvalid,
    GuestPageMapFailed,
    RemotePageReadFailed,
    GuestPageWriteFailed,
    InstructionHookSetupFailed,
};

enum class AlgorithmPositionPacgaSource : std::uint8_t {
    None,
    Oracle,
    Key,
};

// A submission is asynchronous.  Pending means that no new result is
// available for this entity yet; a completed result is consumed once.
enum class AlgorithmPositionRuntimeResult : std::uint8_t {
    Pending,
    Ready,
    Failed,
};

struct AlgorithmPositionRuntimeProbe {
    static constexpr std::size_t kInstructionTraceCapacity = 64;

    AlgorithmPositionRuntimeStage stage =
        AlgorithmPositionRuntimeStage::Idle;
    AlgorithmPositionRuntimeError error =
        AlgorithmPositionRuntimeError::None;
    std::uintptr_t guestPc = 0;
    std::uintptr_t entityAddress = 0;
    std::uintptr_t faultAddress = 0;
    std::uintptr_t finalPc = 0;
    std::uintptr_t expectedPc = 0;
    std::uint64_t requestId = 0;
    std::uint64_t completedRequestId = 0;
    std::uint64_t generation = 0;
    std::uint64_t attempts = 0;
    std::uint64_t successes = 0;
    int unicornError = 0;
    CoordinateReadDiagnostic read{};
    int faultType = 0;
    int faultSize = 0;
    std::int64_t faultValue = 0;
    std::uint64_t stackPointer = 0;
    std::uint64_t x8 = 0;
    std::uint64_t x9 = 0;
    std::uint64_t x10 = 0;
    std::uint64_t x23 = 0;
    std::uint64_t x26 = 0;
    std::uint64_t x27 = 0;
    std::uint64_t pacgaAddress = 0;
    std::uint64_t pacgaData = 0;
    std::uint64_t pacgaModifier = 0;
    std::uint64_t pacgaResult = 0;
    std::uint64_t pacgaCount = 0;
    AlgorithmPositionPacgaSource pacgaSource =
        AlgorithmPositionPacgaSource::None;
    std::uint64_t tpidrEl0 = 0;
    std::uint64_t ctrEl0 = 0;
    std::uint64_t cntfrqEl0 = 0;
    std::uint64_t counterFirst = 0;
    std::uint64_t counterLast = 0;
    std::uint64_t ctrReadCount = 0;
    std::uint64_t tpidrReadCount = 0;
    std::uint64_t cntfrqReadCount = 0;
    std::uint64_t counterReadCount = 0;
    std::array<std::uint64_t, kInstructionTraceCapacity>
        instructionTrace{};
    std::size_t instructionTraceCount = 0;
};

std::uint64_t ComputeAlgorithmPositionPacga(
    std::uint64_t data,
    std::uint64_t modifier,
    const AlgorithmPacgaKey& key) noexcept;

inline constexpr std::uint64_t kAlgorithmPositionPointerPayloadMask =
    UINT64_C(0x0000FFFFFFFFFFFF);

constexpr std::uint64_t NormalizeAlgorithmPositionRemoteAddress(
    std::uint64_t address) noexcept {
    return address & kAlgorithmPositionPointerPayloadMask;
}

constexpr std::uint64_t FormatAlgorithmPacgaResult(
    std::uint64_t computedPac) noexcept {
    return computedPac & UINT64_C(0xFFFFFFFF00000000);
}

struct AlgorithmPositionRuntimeConfig {
    std::uintptr_t decryptRva = 0;
    std::uintptr_t absoluteGuestPc = 0;

    constexpr bool HasRelativeEntry() const noexcept {
        return decryptRva >= 4 &&
            decryptRva <= std::numeric_limits<std::uint32_t>::max() &&
            (decryptRva & 3U) == 0;
    }

    constexpr bool HasAbsoluteEntry() const noexcept {
        return absoluteGuestPc >= 4 &&
            absoluteGuestPc <= kAlgorithmPositionPointerPayloadMask &&
            (absoluteGuestPc & 3U) == 0;
    }

    constexpr bool IsConfigured() const noexcept {
        return HasRelativeEntry() != HasAbsoluteEntry();
    }
};

inline AlgorithmPositionRuntimeConfig ParseAlgorithmPositionDecryptRva(
    std::string_view value) noexcept {
    if (value.size() > 2 && value[0] == '0' &&
        (value[1] == 'x' || value[1] == 'X')) {
        value.remove_prefix(2);
    }
    if (value.empty()) return {};

    std::uintptr_t decryptRva = 0;
    const auto parsed = std::from_chars(
        value.data(), value.data() + value.size(), decryptRva, 16);
    const AlgorithmPositionRuntimeConfig config{decryptRva, 0};
    return parsed.ec == std::errc{} &&
            parsed.ptr == value.data() + value.size() &&
            config.IsConfigured()
        ? config
        : AlgorithmPositionRuntimeConfig{};
}

inline AlgorithmPositionRuntimeConfig ParseAlgorithmPositionGuestPc(
    std::string_view value) noexcept {
    if (value.size() > 2 && value[0] == '0' &&
        (value[1] == 'x' || value[1] == 'X')) {
        value.remove_prefix(2);
    }
    if (value.empty()) return {};

    std::uintptr_t guestPc = 0;
    const auto parsed = std::from_chars(
        value.data(), value.data() + value.size(), guestPc, 16);
    const AlgorithmPositionRuntimeConfig config{0, guestPc};
    return parsed.ec == std::errc{} &&
            parsed.ptr == value.data() + value.size() &&
            config.IsConfigured()
        ? config
        : AlgorithmPositionRuntimeConfig{};
}

constexpr bool ResolveAlgorithmPositionGuestPc(
    const AlgorithmPositionRuntimeConfig& config,
    std::uintptr_t moduleBase,
    std::uintptr_t& guestPc) noexcept {
    guestPc = 0;
    if (!config.IsConfigured()) return false;
    if (config.HasAbsoluteEntry()) {
        guestPc = config.absoluteGuestPc;
        return true;
    }
    if (moduleBase == 0 ||
        config.decryptRva >
            std::numeric_limits<std::uintptr_t>::max() - moduleBase) {
        return false;
    }
    const std::uintptr_t resolved = moduleBase + config.decryptRva;
    if (resolved > kAlgorithmPositionPointerPayloadMask ||
        (resolved & 3U) != 0) {
        return false;
    }
    guestPc = resolved;
    return true;
}

constexpr bool ShouldAttemptAlgorithmPosition(
    bool localActor,
    bool coordinateDecryptRequested,
    bool executionContextReady,
    const AlgorithmPositionRuntimeConfig& config) noexcept {
    return !localActor && coordinateDecryptRequested &&
        executionContextReady && config.IsConfigured();
}

constexpr bool ShouldRequireAlgorithmPosition(
    bool localActor,
    bool coordinateDecryptRequested) noexcept {
    return !localActor && coordinateDecryptRequested;
}

struct AlgorithmPosition {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

class AlgorithmPositionRuntime final {
public:
    AlgorithmPositionRuntime();
    ~AlgorithmPositionRuntime();

    AlgorithmPositionRuntime(const AlgorithmPositionRuntime&) = delete;
    AlgorithmPositionRuntime& operator=(const AlgorithmPositionRuntime&) =
        delete;

    bool Execute(MemoryTransport& memory,
                 std::uintptr_t moduleBase,
                 std::uintptr_t decryptRva,
                 std::uintptr_t entityAddress,
                 std::uint64_t tpidrEl0,
                 const AlgorithmPacgaKey& pacgaKey,
                 AlgorithmPosition& position,
                 bool refreshCachedPages = true) noexcept;
    bool ExecuteAtGuestPc(MemoryTransport& memory,
                          std::uintptr_t guestPc,
                          std::uintptr_t entityAddress,
                          std::uint64_t tpidrEl0,
                          const AlgorithmPacgaKey& pacgaKey,
                          AlgorithmPosition& position,
                          bool refreshCachedPages = true) noexcept;
    AlgorithmPositionRuntimeResult ExecuteAtGuestPcResult(
        MemoryTransport& memory,
        std::uintptr_t guestPc,
        std::uintptr_t entityAddress,
        std::uint64_t tpidrEl0,
        const AlgorithmPacgaKey& pacgaKey,
        const AlgorithmPacgaOracle& pacgaOracle,
        AlgorithmPosition& position,
        bool refreshCachedPages = true) noexcept;
    AlgorithmPositionRuntimeResult ExecuteAtGuestPcResult(
        MemoryTransport& memory,
        std::uintptr_t guestPc,
        std::uintptr_t entityAddress,
        const ProcessExecutionContext& executionContext,
        AlgorithmPosition& position,
        bool refreshCachedPages = true) noexcept;
    bool ExecuteAtGuestPc(MemoryTransport& memory,
                          std::uintptr_t guestPc,
                          std::uintptr_t entityAddress,
                          std::uint64_t tpidrEl0,
                          const AlgorithmPacgaKey& pacgaKey,
                          const AlgorithmPacgaOracle& pacgaOracle,
                          AlgorithmPosition& position,
                          bool refreshCachedPages = true) noexcept;
    AlgorithmPositionRuntimeProbe Probe() const noexcept;
    void Invalidate() noexcept;
    void Reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lengjing::game::native
