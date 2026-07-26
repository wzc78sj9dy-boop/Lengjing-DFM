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
inline constexpr std::uint64_t kCoordinateExecutionSyntheticStackBase =
    UINT64_C(0x00000001FFF00000);
inline constexpr std::uint64_t kCoordinateExecutionSyntheticStackTop =
    UINT64_C(0x0000000200000000);
inline constexpr std::uint64_t kCoordinateExecutionDefaultFrame =
    UINT64_C(0x00000001FFFF7000);
inline constexpr std::uint64_t kCoordinateExecutionDefaultSp =
    UINT64_C(0x00000001FFFF8000);
inline constexpr std::uint64_t kCoordinateExecutionDefaultFp =
    UINT64_C(0x00000001FFFF7F00);
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
    CoordinateExecutionLayout layout{};
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
    bool hookInitialized = false;
    bool returnStubMagicVerifiedBeforeRun = false;
    bool captureValid = false;
    std::uint64_t inputSubject = 0;
    std::uint64_t hookCount = 0;
    std::uint64_t hookX0 = 0;
    std::uint64_t hookX1 = 0;
    std::uint64_t hookX2 = 0;
    std::uint64_t hookSlotValue = 0;
    std::uint64_t hookSnapshotX1 = 0;
    std::uint64_t hookSnapshotX2 = 0;
    std::uint64_t subjectLoadCount = 0;
    std::uint64_t subjectLoadX11 = 0;
    std::uint64_t subjectLoadAddress = 0;
    std::uint64_t subjectLoadValue = 0;
    std::uint64_t callbackCount = 0;
    std::uint64_t callbackX0 = 0;
    std::uint64_t callbackX1 = 0;
    std::uint64_t callbackX2 = 0;
    std::uint64_t callbackTarget = 0;
    bool callbackX1SnapshotValid = false;
    std::uint64_t callbackX1Value0 = 0;
    std::uint64_t callbackX1Value8 = 0;
    std::uint64_t callbackX1Value10 = 0;
    std::uint64_t callbackReturnCount = 0;
    std::uint64_t callbackReturnX0 = 0;
    std::uint64_t callbackIndexCount = 0;
    std::uint64_t callbackIndex = 0;
    std::uint64_t callbackTableProbeCount = 0;
    std::uint64_t callbackTablePointer = 0;
    std::uint64_t callbackTableIndex = 0;
    std::uint64_t callbackTableValue = 0;
    std::uint64_t callbackMutexRecord = 0;
    std::uint64_t callbackLockTarget = 0;
    std::uint32_t callbackMutexBeforeLock = 0;
    std::uint64_t callbackLockReturn = 0;
    std::uint32_t callbackMutexAfterLock = 0;
    std::uint32_t callbackMutexBeforeUnlock = 0;
    std::uint32_t callbackMutexAfterUnlock = 0;
    std::uint64_t callbackFirstCallCount = 0;
    std::uint64_t callbackFirstTarget = 0;
    std::uint64_t callbackFirstArgument = 0;
    std::uint64_t callbackFirstReturnCount = 0;
    std::uint64_t callbackFirstReturn = 0;
    std::uint64_t callbackExternalCallCount = 0;
    std::uint64_t callbackExternalTarget = 0;
    std::uint64_t callbackExternalX0 = 0;
    std::uint64_t callbackExternalX1 = 0;
    std::uint64_t callbackExternalX2 = 0;
    std::uint64_t callbackExternalX3 = 0;
    std::uint64_t callbackExternalReturnCount = 0;
    std::uint64_t callbackExternalReturn = 0;
    std::uint32_t callbackExternalExpected = 0;
    std::uint32_t callbackExternalPriorGate = 0;
    std::uint64_t callbackPrimaryGateWriteCount = 0;
    std::uint32_t callbackPrimaryGateWriteValue = 0;
    std::uint32_t callbackPrimaryGateSource = 0;
    std::uint64_t callbackAlternateGateWriteCount = 0;
    std::uint32_t callbackAlternateGateWriteValue = 0;
    std::uint64_t callbackGateProbeCount = 0;
    std::uint8_t callbackGateFlag = 0;
    std::int32_t callbackGateSnapshotA = 0;
    std::int32_t callbackGateSnapshotB = 0;
    std::uint32_t callbackGateState = 0;
    std::uint32_t callbackRecordCount = 0;
    std::uint64_t callbackTargetKey = 0;
    std::uint64_t callbackRingBase = 0;
    std::uint64_t callbackRingIndexArray = 0;
    std::uint64_t callbackRingProbeCount = 0;
    std::uint64_t callbackRingRowKey = 0;
    std::int64_t callbackRingRowIndex = 0;
    std::int32_t callbackRingMid = 0;
    std::uint64_t callbackRingHitCount = 0;
    std::uint64_t callbackRingHitRow = 0;
    std::uint64_t callbackDispatchCount = 0;
    std::uint64_t callbackDispatchTarget = 0;
    std::uint64_t callbackDispatchArgument = 0;
    std::uint64_t callbackPoolIndexBefore = 0;
    std::uint64_t callbackDispatchReturnCount = 0;
    std::uint64_t callbackDispatchReturn = 0;
    std::uint64_t callbackPoolIndexAfter = 0;
    std::uint64_t callbackResultCount = 0;
    std::uint64_t callbackResultIndex = 0;
    std::uint64_t callbackResultBase = 0;
    std::uint64_t callbackResultPointer = 0;
    bool callbackResultPositionValid = false;
    std::uint32_t callbackResultPositionX = 0;
    std::uint32_t callbackResultPositionY = 0;
    std::uint32_t callbackResultPositionZ = 0;
    std::uint64_t callbackCopyPrepareCount = 0;
    std::uint64_t callbackCopySource = 0;
    std::uint64_t callbackCopyDestination = 0;
    std::uint64_t callbackCopySourceValue0 = 0;
    std::uint64_t callbackCopySourceValue8 = 0;
    std::uint64_t callbackCopySourceValue10 = 0;
    std::uint64_t callbackCopySourceValue18 = 0;
    std::uint64_t callbackCopyAfterCount = 0;
    std::uint64_t callbackCopyDestinationValue0 = 0;
    std::uint64_t callbackCopyDestinationValue8 = 0;
    std::uint64_t callbackCopyDestinationValue10 = 0;
    std::uint64_t callbackCopyDestinationValue18 = 0;
    std::uint64_t exclusiveLoadCount = 0;
    std::uint64_t exclusiveClearCount = 0;
    std::uint64_t exclusiveStoreCount = 0;
    std::uint64_t exclusiveStoreFailureCount = 0;
    std::uint32_t lastExclusiveInstruction = 0;
    std::uint64_t svcCount = 0;
    std::uint64_t svcNumber0 = 0;
    std::uint64_t svcNumber1 = 0;
    std::uint64_t svcNumber2 = 0;
    std::uint64_t svcNumber3 = 0;
    std::uint64_t lastSvcNumber = 0;
    std::uint32_t exclusiveStoreStatusRegister = 0;
    std::uint64_t descriptorEndQueryCount = 0;
    std::int32_t descriptorEndQueryFd = -1;
    std::int64_t descriptorEndQueryResult = -1;
    std::uint64_t taggedBaseRewriteCount = 0;
    std::uint32_t taggedBaseRegister = 0;
    std::uint64_t taggedBaseBefore = 0;
    std::uint64_t taggedBaseAfter = 0;
    std::uint64_t pacgaCount = 0;
    std::uint64_t lastPacgaSource = 0;
    std::uint64_t lastPacgaModifier = 0;
    std::uint64_t lastPacgaResult = 0;
    std::uint64_t seedSubject = 0;
    std::uint64_t seedSlot = 0;
    std::uint64_t captureCount = 0;
    std::uint64_t capturedPc = 0;
    std::uint64_t capturedSlot = 0;
    std::uint64_t capturedObject = 0;
    std::uint64_t capturedSp = 0;
    std::uint64_t capturedX8 = 0;
    std::uint64_t capturedX9 = 0;
    std::uint64_t capturedX12 = 0;
    std::uint64_t capturedX21 = 0;
    std::uint64_t capturedLocal0 = 0;
    std::uint64_t capturedLocal1 = 0;
    std::uint64_t capturedLocal2 = 0;
    std::uint64_t capturedLocal3 = 0;
    std::uint32_t capturedLocalField = 0;
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

constexpr bool HasRecordedCoordinateExecutionSvc(
    const CoordinateExecutionEvidence& evidence,
    std::uint64_t number) noexcept {
    return (evidence.svcCount >= 1 && evidence.svcNumber0 == number) ||
        (evidence.svcCount >= 2 && evidence.svcNumber1 == number) ||
        (evidence.svcCount >= 3 && evidence.svcNumber2 == number) ||
        (evidence.svcCount >= 4 && evidence.svcNumber3 == number);
}

constexpr std::uint64_t NormalizeCoordinateExecutionPointer(
    std::uint64_t value) noexcept {
    return value & kCoordinateExecutionPointerMask;
}

constexpr bool IsCoordinateExecutionTaggedMemoryInstruction(
    std::uint32_t instruction) noexcept {
    return (instruction & UINT32_C(0x3B000000)) ==
            UINT32_C(0x39000000) ||
        (instruction & UINT32_C(0x3B200C00)) ==
            UINT32_C(0x38200800) ||
        (instruction & UINT32_C(0x3B200C00)) ==
            UINT32_C(0x38200000);
}

constexpr std::uint32_t CoordinateExecutionMemoryBaseRegister(
    std::uint32_t instruction) noexcept {
    return (instruction >> 5U) & UINT32_C(0x1F);
}

constexpr bool IsCoordinateExecutionLoadExclusiveInstruction(
    std::uint32_t instruction) noexcept {
    switch (instruction & UINT32_C(0xFFE0FC00)) {
        case UINT32_C(0x88407C00):
        case UINT32_C(0x8840FC00):
        case UINT32_C(0x88607C00):
        case UINT32_C(0x8860FC00):
        case UINT32_C(0xC8407C00):
        case UINT32_C(0xC840FC00):
        case UINT32_C(0xC8607C00):
        case UINT32_C(0xC860FC00):
            return true;
        default:
            return false;
    }
}

constexpr bool IsCoordinateExecutionStoreExclusiveInstruction(
    std::uint32_t instruction) noexcept {
    switch (instruction & UINT32_C(0xFFE0FC00)) {
        case UINT32_C(0x88007C00):
        case UINT32_C(0x8800FC00):
        case UINT32_C(0x88207C00):
        case UINT32_C(0x8820FC00):
        case UINT32_C(0xC8007C00):
        case UINT32_C(0xC800FC00):
        case UINT32_C(0xC8207C00):
        case UINT32_C(0xC820FC00):
            return true;
        default:
            return false;
    }
}

constexpr bool IsCoordinateExecutionClearExclusiveInstruction(
    std::uint32_t instruction) noexcept {
    return (instruction & UINT32_C(0xFFFFF0FF)) ==
        UINT32_C(0xD503305F);
}

constexpr std::uint32_t CoordinateExecutionStoreExclusiveStatusRegister(
    std::uint32_t instruction) noexcept {
    return (instruction >> 16U) & UINT32_C(0x1F);
}

constexpr bool CoordinateExecutionExclusiveMonitorInvalidAfterInstruction(
    bool current,
    std::uint32_t instruction) noexcept {
    if (IsCoordinateExecutionLoadExclusiveInstruction(instruction)) {
        return false;
    }
    if (IsCoordinateExecutionClearExclusiveInstruction(instruction) ||
        IsCoordinateExecutionStoreExclusiveInstruction(instruction)) {
        return true;
    }
    return current;
}

constexpr bool IsCoordinateExecutionCanonicalFaultBase(
    std::uint64_t value) noexcept {
    return (value >> 37U) >= 3U && value < UINT64_C(0x8000000001);
}

constexpr bool ShouldRedirectCoordinateExecutionReturn(
    const CoordinateExecutionPlan& plan,
    std::uint64_t pc) noexcept {
    return plan.returnStub != 0 &&
        NormalizeCoordinateExecutionPointer(pc) ==
            NormalizeCoordinateExecutionPointer(plan.returnStub);
}

constexpr bool IsCoordinateExecutionPointer(std::uint64_t value) noexcept {
    const std::uint64_t pointer =
        NormalizeCoordinateExecutionPointer(value);
    return pointer >= kCoordinateExecutionPointerMin &&
        pointer <= kCoordinateExecutionPointerMax;
}

constexpr bool IsCoordinateExecutionStackBase(std::uint64_t value) noexcept {
    const std::uint64_t pointer =
        NormalizeCoordinateExecutionPointer(value);
    return IsCoordinateExecutionPointer(pointer) ||
        (pointer >= kCoordinateExecutionSyntheticStackBase &&
         pointer < kCoordinateExecutionSyntheticStackTop);
}

constexpr bool ShouldInitializeCoordinateExecutionHook(
    bool initialized,
    std::uint64_t address,
    std::uint64_t hookPc) noexcept {
    return !initialized && address == hookPc;
}

constexpr std::uint64_t CoordinateExecutionSvcResult(
    std::uint64_t number) noexcept {
    return number == 172 || number == 178 ? 1 : 0;
}

constexpr bool IsCoordinateExecutionDescriptorEndQuery(
    std::uint64_t number,
    std::uint64_t offset,
    std::uint64_t whence) noexcept {
    return number == 62 && offset == 0 && whence == 2;
}

constexpr bool IsCoordinateExecutionDescriptorEndSvc(
    std::uint64_t number,
    std::uint64_t offset,
    std::uint64_t whence) noexcept {
    return IsCoordinateExecutionDescriptorEndQuery(number, offset, whence);
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
    if (!IsCoordinateExecutionMode(request.mode) || !request.layout.IsValid() ||
        moduleBase == 0 ||
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
        plan.x0 = subject;
        plan.x1 = subject;
        plan.lr = returnStub;
        plan.returnStub = returnStub;
    } else {
        plan.x0 = request.shared.x0Override != 0
            ? NormalizeCoordinateExecutionPointer(
                  request.shared.x0Override)
            : subject;
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
