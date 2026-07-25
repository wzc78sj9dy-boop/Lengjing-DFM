#pragma once

#include "game/CoordinateDecryptDiagnostics.h"
#include "game/native/CoordinateExecutionCandidate.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

namespace lengjing::game::native {

class MemoryTransport;
struct ProcessExecutionContext;

inline constexpr std::uint64_t kCoordinateExecutionStopPc =
    UINT64_C(0x0000000400000000);
inline constexpr std::uint64_t kCoordinateExecutionDefaultFrame =
    UINT64_C(0x00000001FFFF7000);
inline constexpr std::uint64_t kCoordinateExecutionDefaultSp =
    UINT64_C(0x00000001FFFF8000);
inline constexpr std::uint64_t kCoordinateExecutionDefaultFp =
    UINT64_C(0x00000001FFFF7F00);
inline constexpr std::uint64_t kCoordinateExecutionResultSlotOffset = 0x200;
inline constexpr std::uint64_t kCoordinateExecutionPositionOffset = 0x168;
inline constexpr std::uint64_t kCoordinateExecutionReturnStubMax =
    UINT64_C(0x0000007FFFFFFFE0);

enum class CoordinateExecutionMode : std::uint8_t {
    Emulate = 1,
    Interpret = 2,
    Predecode = 3,
    Jit = 4,
};

enum class CoordinateExecutionStatus : std::uint8_t {
    Idle = 0,
    Loading = 1,
    Success = 2,
    EnvironmentFailure = 3,
    InvalidAddress = 4,
    BackendUnavailable = 5,
    EvidenceFailure = 6,
    InvalidObject = 7,
    ReadOrCoordinateFailure = 8,
};

enum class CoordinateExecutionRuntimeStage : std::uint8_t {
    Idle,
    Preparing,
    Executing,
    Validating,
    Completed,
    Failed,
};

enum class CoordinateExecutionRuntimeError : std::uint16_t {
    None = 0,
    InvalidRequest,
    ReturnStubInvalid,
    EngineSetupFailed,
    RegisterSetupFailed,
    RemotePageReadFailed,
    GuestPageMapFailed,
    GuestPageWriteFailed,
    InstructionHookSetupFailed,
    PacgaUnavailable,
    UnsupportedSvc,
    EmulationFailed,
    ReturnPcMismatch,
    EvidenceInvalid,
    ResultReadFailed,
    ResultInvalid,
};

#pragma pack(push, 1)
struct CoordinateExecutionPosition {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

struct CoordinateExecutionResult {
    std::uint8_t ok = 0;
    CoordinateExecutionStatus status = CoordinateExecutionStatus::Idle;
    std::uint16_t reserved = 0;
    CoordinateExecutionPosition position{};
    std::uint64_t object = 0;
};
#pragma pack(pop)

static_assert(sizeof(CoordinateExecutionPosition) == 12);
static_assert(sizeof(CoordinateExecutionResult) == 24);
static_assert(offsetof(CoordinateExecutionResult, position) == 4);
static_assert(offsetof(CoordinateExecutionResult, object) == 0x10);

struct CoordinateExecutionSharedEntry {
    std::uint64_t hookOffset = 0;
    std::uint64_t x0Override = 0;
    std::uint64_t absoluteEntry = 0;
    std::uint64_t returnStub = 0;
};

struct CoordinateExecutionRequest {
    CoordinateExecutionMode mode = CoordinateExecutionMode::Interpret;
    CoordinateExecutionCandidate candidate{};
    CoordinateExecutionSharedEntry shared{};
    bool candidateKnown = false;
};

struct CoordinateExecutionPlan {
    bool valid = false;
    std::uint64_t entryPc = 0;
    std::uint64_t hookPc = 0;
    std::uint64_t returnStub = 0;
    std::uint64_t x0 = 0;
    std::uint64_t x1 = 0;
    std::uint64_t x2 = 0;
    std::uint64_t lr = 0;
    std::uint64_t expectedStackBase = 0;
    std::uint64_t timeoutMicros = 0;
    std::uint64_t instructionBudget = 0;
    bool seedSlotBeforeRun = false;
    bool seedSlotAtHook = false;
    bool requireReturnStub = false;
    bool verifyReturnStubMagic = false;
};

struct CoordinateExecutionEvidence {
    bool hitStopPc = false;
    bool hitReturnStub = false;
    bool hitHookPc = false;
    bool returnStubMagicVerifiedBeforeRun = false;
    bool captureValid = false;
    std::uint64_t captureCount = 0;
    std::uint64_t capturedPc = 0;
    std::uint64_t capturedSlot = 0;
    std::uint64_t capturedObject = 0;
    std::uint64_t stackBase = 0;
    std::uint64_t finalPc = 0;
    std::size_t capturedWriteSize = 0;
};

struct CoordinateExecutionRuntimeProbe {
    CoordinateExecutionRuntimeStage stage =
        CoordinateExecutionRuntimeStage::Idle;
    CoordinateExecutionRuntimeError error =
        CoordinateExecutionRuntimeError::None;
    CoordinateExecutionStatus status = CoordinateExecutionStatus::Idle;
    CoordinateExecutionMode mode = CoordinateExecutionMode::Interpret;
    std::uintptr_t faultAddress = 0;
    int faultType = 0;
    int faultSize = 0;
    std::int64_t faultValue = 0;
    int unicornError = 0;
    game::CoordinateReadDiagnostic read{};
    CoordinateExecutionPlan plan{};
    CoordinateExecutionEvidence evidence{};
};

constexpr std::uint64_t NormalizeCoordinateExecutionPointer(
    std::uint64_t value) noexcept {
    return value & kCoordinateExecutionPointerMask;
}

constexpr bool IsCoordinateExecutionPointer(std::uint64_t value) noexcept {
    const std::uint64_t pointer =
        NormalizeCoordinateExecutionPointer(value);
    return pointer >= kCoordinateExecutionPointerMin &&
        pointer <= kCoordinateExecutionPointerMax;
}

constexpr bool IsCoordinateExecutionMode(CoordinateExecutionMode mode) noexcept {
    const auto value = static_cast<std::uint8_t>(mode);
    return value >= static_cast<std::uint8_t>(
                        CoordinateExecutionMode::Emulate) &&
        value <= static_cast<std::uint8_t>(CoordinateExecutionMode::Jit);
}

constexpr bool CoordinateExecutionAdd(std::uint64_t left,
                                      std::uint64_t right,
                                      std::uint64_t& output) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        output = 0;
        return false;
    }
    output = left + right;
    return true;
}

constexpr bool ContainsCoordinateExecutionCodeAddress(
    std::uint64_t moduleBase,
    std::size_t moduleSize,
    std::uint64_t address,
    std::size_t requiredBytes = sizeof(std::uint32_t)) noexcept {
    if (requiredBytes == 0 || moduleSize < requiredBytes) return false;
    std::uint64_t last = 0;
    return CoordinateExecutionAdd(
               moduleBase,
               static_cast<std::uint64_t>(moduleSize - requiredBytes),
               last) &&
        address >= moduleBase && address <= last && (address & 3U) == 0;
}

constexpr CoordinateExecutionPlan BuildCoordinateExecutionPlan(
    std::uint64_t moduleBase,
    std::size_t moduleSize,
    std::uint64_t codeBase,
    std::size_t codeSize,
    std::uint64_t subject,
    const CoordinateExecutionRequest& request) noexcept {
    CoordinateExecutionPlan plan{};
    if (!IsCoordinateExecutionMode(request.mode) || moduleBase == 0 ||
        moduleSize == 0 || codeBase == 0 || codeSize == 0 ||
        !IsCoordinateExecutionPointer(subject)) {
        return plan;
    }

    subject = NormalizeCoordinateExecutionPointer(subject);
    if (request.mode == CoordinateExecutionMode::Emulate) {
        const auto& candidate = request.candidate;
        if (candidate.q1 == 0 ||
            (request.candidateKnown && candidate.q0 == 0) ||
            !CoordinateExecutionAdd(codeBase, candidate.q1, plan.hookPc) ||
            !ContainsCoordinateExecutionCodeAddress(
                codeBase, codeSize, plan.hookPc)) {
            return CoordinateExecutionPlan{};
        }

        const std::uint64_t q0 =
            NormalizeCoordinateExecutionPointer(candidate.q0);
        const std::uint64_t alternateEntry =
            NormalizeCoordinateExecutionPointer(candidate.q2);
        const std::uint64_t returnStub =
            NormalizeCoordinateExecutionPointer(candidate.q3);
        if (alternateEntry != 0) {
            if (!ContainsCoordinateExecutionCodeAddress(
                    moduleBase, moduleSize, returnStub, 12) ||
                returnStub > kCoordinateExecutionReturnStubMax ||
                !IsCoordinateExecutionPointer(returnStub)) {
                return CoordinateExecutionPlan{};
            }
            plan.entryPc = alternateEntry;
            plan.x0 = subject;
            plan.x1 = subject;
            plan.x2 = subject;
            plan.lr = returnStub;
            plan.returnStub = returnStub;
            plan.requireReturnStub = true;
            plan.verifyReturnStubMagic = true;
        } else {
            plan.entryPc = plan.hookPc;
            plan.lr = kCoordinateExecutionStopPc;
            plan.x2 = subject;
            if (q0 != 0) {
                plan.x0 = q0;
                plan.x1 = kCoordinateExecutionDefaultFrame;
                plan.expectedStackBase = kCoordinateExecutionDefaultFrame;
                plan.seedSlotBeforeRun = true;
            } else {
                plan.x0 = subject;
                plan.x1 = subject;
            }
        }
        plan.timeoutMicros = request.candidateKnown ? 120000 : 45000;
        plan.instructionBudget = request.candidateKnown ? 3000000 : 600000;
        plan.valid = true;
        return plan;
    }

    if (request.mode != CoordinateExecutionMode::Interpret &&
        request.mode != CoordinateExecutionMode::Predecode &&
        request.mode != CoordinateExecutionMode::Jit) {
        return plan;
    }
    if (!CoordinateExecutionAdd(
            codeBase, request.shared.hookOffset, plan.hookPc) ||
        !ContainsCoordinateExecutionCodeAddress(
            codeBase, codeSize, plan.hookPc)) {
        return CoordinateExecutionPlan{};
    }

    plan.x0 = request.shared.x0Override != 0
        ? NormalizeCoordinateExecutionPointer(request.shared.x0Override)
        : subject;
    plan.x2 = subject;
    plan.seedSlotAtHook = true;
    if (request.shared.absoluteEntry != 0) {
        const std::uint64_t returnStub =
            NormalizeCoordinateExecutionPointer(request.shared.returnStub);
        if (!ContainsCoordinateExecutionCodeAddress(
                moduleBase, moduleSize, returnStub, 12)) {
            return CoordinateExecutionPlan{};
        }
        plan.entryPc = NormalizeCoordinateExecutionPointer(
            request.shared.absoluteEntry);
        if ((plan.entryPc & 3U) != 0 || plan.entryPc == 0) {
            return CoordinateExecutionPlan{};
        }
        plan.x1 = subject;
        plan.lr = returnStub;
        plan.returnStub = returnStub;
    } else {
        plan.entryPc = plan.hookPc;
        plan.x1 = kCoordinateExecutionDefaultFrame;
        plan.lr = kCoordinateExecutionStopPc;
        plan.expectedStackBase = kCoordinateExecutionDefaultFrame;
        plan.seedSlotBeforeRun = true;
    }
    plan.timeoutMicros = 20000;
    plan.instructionBudget = 4000000;
    plan.valid = true;
    return plan;
}

class CoordinateExecutionRuntime final {
public:
    CoordinateExecutionRuntime();
    ~CoordinateExecutionRuntime();

    CoordinateExecutionRuntime(const CoordinateExecutionRuntime&) = delete;
    CoordinateExecutionRuntime& operator=(
        const CoordinateExecutionRuntime&) = delete;

    CoordinateExecutionResult Execute(
        MemoryTransport& memory,
        std::uintptr_t moduleBase,
        std::size_t moduleSize,
        std::uintptr_t codeBase,
        std::size_t codeSize,
        std::uintptr_t subject,
        const ProcessExecutionContext& executionContext,
        const CoordinateExecutionRequest& request) noexcept;

    CoordinateExecutionRuntimeProbe Probe() const noexcept;
    void Reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lengjing::game::native
