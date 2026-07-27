#include "game/native/CoordinateExecutionRuntime.h"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace {

#define REQUIRE(condition)                                                    \
    do {                                                                      \
        if (!(condition)) {                                                   \
            throw std::runtime_error(                                         \
                std::string(__FILE__) + ":" + std::to_string(__LINE__) +     \
                ": requirement failed: " #condition);                       \
        }                                                                     \
    } while (false)

using lengjing::game::native::BuildCoordinateExecutionPlan;
using lengjing::game::native::CoordinateExecutionMode;
using lengjing::game::native::CoordinateExecutionMemoryBaseRegister;
using lengjing::game::native::
    CoordinateExecutionExclusiveMonitorInvalidAfterInstruction;
using lengjing::game::native::CoordinateExecutionBranchKind;
using lengjing::game::native::CoordinateExecutionStoreExclusiveStatusRegister;
using lengjing::game::native::CoordinateExecutionRequest;
using lengjing::game::native::CoordinateExecutionStatus;
using lengjing::game::native::ContainsCoordinateExecutionCodeAddress;
using lengjing::game::native::CoordinateExecutionSvcResult;
using lengjing::game::native::DecodeCoordinateExecutionBranch;
using lengjing::game::native::EvaluateCoordinateExecutionDescriptorSeek;
using lengjing::game::native::IsCoordinateExecutionPointer;
using lengjing::game::native::IsCoordinateExecutionCanonicalFaultBase;
using lengjing::game::native::IsCoordinateExecutionClearExclusiveInstruction;
using lengjing::game::native::IsCoordinateExecutionDescriptorEndQuery;
using lengjing::game::native::IsCoordinateExecutionLoadExclusiveInstruction;
using lengjing::game::native::IsCoordinateExecutionStackBase;
using lengjing::game::native::IsCoordinateExecutionStoreExclusiveInstruction;
using lengjing::game::native::IsCoordinateExecutionTaggedMemoryInstruction;
using lengjing::game::native::NormalizeCoordinateExecutionPointer;
using lengjing::game::native::
    ResolveCoordinateExecutionImmediateBranchTarget;
using lengjing::game::native::ShouldInitializeCoordinateExecutionHook;
using lengjing::game::native::ShouldRedirectCoordinateExecutionReturn;
using lengjing::game::native::kCoordinateExecutionDefaultFrame;
using lengjing::game::native::kCoordinateExecutionDefaultFp;
using lengjing::game::native::kCoordinateExecutionDefaultSp;
using lengjing::game::native::kCoordinateExecutionStopPc;
using lengjing::game::native::kCoordinateExecutionSyntheticStackBase;
using lengjing::game::native::kCoordinateExecutionSyntheticStackTop;

constexpr std::uint64_t kModuleBase = UINT64_C(0x0000007000000000);
constexpr std::size_t kModuleSize = 0x200000;
constexpr std::uint64_t kCodeBase = UINT64_C(0x0000007100000000);
constexpr std::size_t kCodeSize = 0x100000;
constexpr std::uint64_t kSubject = UINT64_C(0xAB00007010000000);
constexpr std::uint64_t kNormalizedSubject = UINT64_C(0x0000007010000000);

constexpr lengjing::game::native::CoordinateExecutionLayout
SyntheticLayout() {
    lengjing::game::native::CoordinateExecutionLayout layout{};
    layout.discovery = {
        UINT64_C(0x234000),
        UINT64_C(0x18),
        UINT64_C(0xB8),
        UINT64_C(0x123456789ABCDEF0),
    };
    layout.result = {UINT64_C(0x248), UINT64_C(0x178)};
    layout.hooks = {
        0x410, 0x434, 0x458, 0x47C, 0x4A0, 0x4C4, 0x4E8,
        0x50C, 0x530, 0x554, 0x578, 0x59C, 0x5C0, 0x5E4,
        0x608, 0x62C, 0x650, 0x674, 0x698, 0x6BC, 0x6E0,
        0x704, 0x728, 0x74C, 0x770, 0x794,
    };
    layout.fields = {
        0x818, 0x83C, 0x860, 0x884, 0x8A8,
        0x8CC, 0x8F0, 0x914, 0x938, 0x95C,
        0x980, 0x9A4, 0x9C8, 0x9EC, 0xA10,
    };
    return layout;
}

void TestAbiAndPointers() {
    static_assert(static_cast<std::uint8_t>(
                      CoordinateExecutionStatus::Idle) == 0);
    static_assert(static_cast<std::uint8_t>(
                      CoordinateExecutionStatus::Success) == 2);
    static_assert(static_cast<std::uint8_t>(
                      CoordinateExecutionStatus::ReadOrCoordinateFailure) == 8);
    REQUIRE(NormalizeCoordinateExecutionPointer(kSubject) ==
            kNormalizedSubject);
    REQUIRE(IsCoordinateExecutionPointer(kSubject));
    REQUIRE(!IsCoordinateExecutionPointer(UINT64_C(0x1000)));
    REQUIRE(ContainsCoordinateExecutionCodeAddress(
        kModuleBase, kModuleSize, kModuleBase + 4));
    REQUIRE(!ContainsCoordinateExecutionCodeAddress(
        kModuleBase, kModuleSize, kModuleBase + kModuleSize));
    REQUIRE(kModuleBase + kModuleSize < kCodeBase);
}

void TestSyntheticStackContract() {
    REQUIRE(kCoordinateExecutionSyntheticStackTop -
                kCoordinateExecutionSyntheticStackBase ==
            UINT64_C(0x100000));
    REQUIRE(!IsCoordinateExecutionStackBase(
        kCoordinateExecutionSyntheticStackBase - 1));
    REQUIRE(IsCoordinateExecutionStackBase(
        kCoordinateExecutionSyntheticStackBase));
    REQUIRE(IsCoordinateExecutionStackBase(
        kCoordinateExecutionSyntheticStackTop - 1));
    REQUIRE(!IsCoordinateExecutionStackBase(
        kCoordinateExecutionSyntheticStackTop));
    REQUIRE(IsCoordinateExecutionStackBase(kCoordinateExecutionDefaultFrame));
    REQUIRE(IsCoordinateExecutionStackBase(kCoordinateExecutionDefaultFp));
    REQUIRE(IsCoordinateExecutionStackBase(kCoordinateExecutionDefaultSp));
    REQUIRE(kCoordinateExecutionDefaultFrame +
                SyntheticLayout().result.resultSlotOffset <
            kCoordinateExecutionSyntheticStackTop);
    REQUIRE(IsCoordinateExecutionStackBase(kNormalizedSubject));
    REQUIRE(IsCoordinateExecutionStackBase(kSubject));
    REQUIRE(!IsCoordinateExecutionStackBase(UINT64_C(0x1000)));
}

void TestHookInitializationGate() {
    constexpr std::uint64_t kHookPc = kCodeBase + 0x1000;
    REQUIRE(ShouldInitializeCoordinateExecutionHook(false, kHookPc, kHookPc));
    REQUIRE(!ShouldInitializeCoordinateExecutionHook(true, kHookPc, kHookPc));
    REQUIRE(!ShouldInitializeCoordinateExecutionHook(
        false, kHookPc + 4, kHookPc));
}

void TestSvcContract() {
    REQUIRE(CoordinateExecutionSvcResult(25) == 0);
    REQUIRE(CoordinateExecutionSvcResult(29) == 0);
    REQUIRE(CoordinateExecutionSvcResult(56) == 0);
    REQUIRE(CoordinateExecutionSvcResult(62) == 0);
    REQUIRE(CoordinateExecutionSvcResult(96) == 0);
    REQUIRE(CoordinateExecutionSvcResult(98) == 0);
    REQUIRE(CoordinateExecutionSvcResult(160) == 0);
    REQUIRE(CoordinateExecutionSvcResult(172) == 1);
    REQUIRE(CoordinateExecutionSvcResult(178) == 1);
    REQUIRE(CoordinateExecutionSvcResult(278) == 0);
    REQUIRE(CoordinateExecutionSvcResult(UINT64_MAX) == 0);
    REQUIRE(IsCoordinateExecutionDescriptorEndQuery(62, 0, 2));
    REQUIRE(!IsCoordinateExecutionDescriptorEndQuery(62, 1, 2));
    REQUIRE(!IsCoordinateExecutionDescriptorEndQuery(62, 0, 1));
    REQUIRE(!IsCoordinateExecutionDescriptorEndQuery(178, 0, 2));
}

void TestExternalBranchContract() {
    const auto blr = DecodeCoordinateExecutionBranch(
        UINT32_C(0xD63F0100));
    REQUIRE(blr.kind == CoordinateExecutionBranchKind::Call);
    REQUIRE(!blr.immediate);
    REQUIRE(blr.targetRegister == 8);

    const auto br = DecodeCoordinateExecutionBranch(
        UINT32_C(0xD61F0100));
    REQUIRE(br.kind == CoordinateExecutionBranchKind::Jump);
    REQUIRE(!br.immediate);
    REQUIRE(br.targetRegister == 8);

    const auto authenticatedCall = DecodeCoordinateExecutionBranch(
        UINT32_C(0xD63F081F) | (8U << 5U));
    REQUIRE(authenticatedCall.kind == CoordinateExecutionBranchKind::Call);
    REQUIRE(authenticatedCall.targetRegister == 8);
    const auto authenticatedJump = DecodeCoordinateExecutionBranch(
        UINT32_C(0xD61F081F) | (9U << 5U));
    REQUIRE(authenticatedJump.kind == CoordinateExecutionBranchKind::Jump);
    REQUIRE(authenticatedJump.targetRegister == 9);

    const auto authenticatedZeroCall = DecodeCoordinateExecutionBranch(
        (UINT32_C(0x1AE7E1) << 11U) | (10U << 5U));
    REQUIRE(authenticatedZeroCall.kind ==
            CoordinateExecutionBranchKind::Call);
    REQUIRE(authenticatedZeroCall.targetRegister == 10);
    const auto authenticatedZeroJump = DecodeCoordinateExecutionBranch(
        (UINT32_C(0x1AE3E1) << 11U) | (11U << 5U));
    REQUIRE(authenticatedZeroJump.kind ==
            CoordinateExecutionBranchKind::Jump);
    REQUIRE(authenticatedZeroJump.targetRegister == 11);

    constexpr std::uint64_t pc = UINT64_C(0x7000001000);
    const auto forward = DecodeCoordinateExecutionBranch(
        UINT32_C(0x94000002));
    REQUIRE(forward.kind == CoordinateExecutionBranchKind::Call);
    REQUIRE(forward.immediate);
    REQUIRE(ResolveCoordinateExecutionImmediateBranchTarget(
                pc, UINT32_C(0x94000002)) == pc + 8);
    REQUIRE(ResolveCoordinateExecutionImmediateBranchTarget(
                pc, UINT32_C(0x97FFFFFF)) == pc - 4);
    REQUIRE(!DecodeCoordinateExecutionBranch(
                 UINT32_C(0x14000000)).IsValid());
    REQUIRE(!DecodeCoordinateExecutionBranch(
                 UINT32_C(0xD65F03C0)).IsValid());
}

void TestFakeDescriptorSeekContract() {
    const auto empty = EvaluateCoordinateExecutionDescriptorSeek(
        0, 0, 123, 2);
    REQUIRE(empty.result == 0);
    REQUIRE(empty.current == 0);
    REQUIRE(empty.emptyFile);

    const auto absolute = EvaluateCoordinateExecutionDescriptorSeek(
        100, 25, 30, 0);
    REQUIRE(absolute.result == 30);
    REQUIRE(absolute.current == 30);
    REQUIRE(!absolute.emptyFile);

    const auto relative = EvaluateCoordinateExecutionDescriptorSeek(
        100, 25, -10, 1);
    REQUIRE(relative.result == 15);
    REQUIRE(relative.current == 15);

    const auto fromEnd = EvaluateCoordinateExecutionDescriptorSeek(
        100, 25, -5, 2);
    REQUIRE(fromEnd.result == 95);
    REQUIRE(fromEnd.current == 95);

    const auto beforeStart = EvaluateCoordinateExecutionDescriptorSeek(
        100, 25, -26, 1);
    REQUIRE(beforeStart.result == -22);
    REQUIRE(beforeStart.current == 25);
    const auto badWhence = EvaluateCoordinateExecutionDescriptorSeek(
        100, 25, 0, 3);
    REQUIRE(badWhence.result == -22);
    REQUIRE(badWhence.current == 25);
    const auto overflow = EvaluateCoordinateExecutionDescriptorSeek(
        static_cast<std::uint64_t>(INT64_MAX),
        static_cast<std::uint64_t>(INT64_MAX),
        1,
        1);
    REQUIRE(overflow.result == -22);
    REQUIRE(overflow.current == static_cast<std::uint64_t>(INT64_MAX));
}

void TestTaggedMemoryInstructionContract() {
    REQUIRE(IsCoordinateExecutionTaggedMemoryInstruction(
        UINT32_C(4181723400)));
    REQUIRE(CoordinateExecutionMemoryBaseRegister(
                UINT32_C(4181723400)) ==
            8);
    REQUIRE(!IsCoordinateExecutionTaggedMemoryInstruction(
        UINT32_C(0xD503201F)));
    REQUIRE(IsCoordinateExecutionCanonicalFaultBase(
        UINT64_C(0x734D178C00)));
    REQUIRE(!IsCoordinateExecutionCanonicalFaultBase(
        UINT64_C(0x2000000000)));
}

void TestExclusiveMonitorContract() {
    constexpr std::array<std::uint32_t, 8> kLoadExclusiveOpcodes{
        UINT32_C(0x88407C00), UINT32_C(0x8840FC00),
        UINT32_C(0x88607C00), UINT32_C(0x8860FC00),
        UINT32_C(0xC8407C00), UINT32_C(0xC840FC00),
        UINT32_C(0xC8607C00), UINT32_C(0xC860FC00)};
    constexpr std::array<std::uint32_t, 8> kStoreExclusiveOpcodes{
        UINT32_C(0x88007C00), UINT32_C(0x8800FC00),
        UINT32_C(0x88207C00), UINT32_C(0x8820FC00),
        UINT32_C(0xC8007C00), UINT32_C(0xC800FC00),
        UINT32_C(0xC8207C00), UINT32_C(0xC820FC00)};
    for (const std::uint32_t instruction : kLoadExclusiveOpcodes) {
        REQUIRE(IsCoordinateExecutionLoadExclusiveInstruction(instruction));
        REQUIRE(!IsCoordinateExecutionStoreExclusiveInstruction(instruction));
    }
    for (const std::uint32_t instruction : kStoreExclusiveOpcodes) {
        REQUIRE(IsCoordinateExecutionStoreExclusiveInstruction(instruction));
        REQUIRE(!IsCoordinateExecutionLoadExclusiveInstruction(instruction));
    }
    REQUIRE(IsCoordinateExecutionLoadExclusiveInstruction(
        UINT32_C(0x885F7D6A)));
    REQUIRE(IsCoordinateExecutionStoreExclusiveInstruction(
        UINT32_C(0x88097D6A)));
    REQUIRE(CoordinateExecutionStoreExclusiveStatusRegister(
                UINT32_C(0x88097D6A)) ==
            9);
    REQUIRE(IsCoordinateExecutionClearExclusiveInstruction(
        UINT32_C(0xD503305F)));
    REQUIRE(IsCoordinateExecutionClearExclusiveInstruction(
        UINT32_C(0xD5033F5F)));
    REQUIRE(!IsCoordinateExecutionClearExclusiveInstruction(
        UINT32_C(0xD503201F)));

    bool invalid = true;
    invalid = CoordinateExecutionExclusiveMonitorInvalidAfterInstruction(
        invalid, UINT32_C(0x885F7D6A));
    REQUIRE(!invalid);
    invalid = CoordinateExecutionExclusiveMonitorInvalidAfterInstruction(
        invalid, UINT32_C(0xD5033F5F));
    REQUIRE(invalid);
    invalid = CoordinateExecutionExclusiveMonitorInvalidAfterInstruction(
        invalid, UINT32_C(0x885F7D6A));
    REQUIRE(!invalid);
    invalid = CoordinateExecutionExclusiveMonitorInvalidAfterInstruction(
        invalid, UINT32_C(0x88097D6A));
    REQUIRE(invalid);
    REQUIRE(CoordinateExecutionExclusiveMonitorInvalidAfterInstruction(
        invalid, UINT32_C(0xD503201F)));
}

void TestKnownRelativeCandidate() {
    CoordinateExecutionRequest request{};
    request.mode = CoordinateExecutionMode::Emulate;
    request.layout = SyntheticLayout();
    request.candidateKnown = true;
    request.candidate.q0 = UINT64_C(0xCD00007020000000);
    request.candidate.q1 = 0x1000;

    const auto plan = BuildCoordinateExecutionPlan(
        kModuleBase, kModuleSize, kCodeBase, kCodeSize, kSubject, request);
    REQUIRE(plan.valid);
    REQUIRE(plan.entryPc == kCodeBase + 0x1000);
    REQUIRE(plan.hookPc == plan.entryPc);
    REQUIRE(plan.x0 == UINT64_C(0x0000007020000000));
    REQUIRE(plan.x1 == kCoordinateExecutionDefaultFrame);
    REQUIRE(plan.x2 == kNormalizedSubject);
    REQUIRE(plan.lr == kCoordinateExecutionStopPc);
    REQUIRE(plan.expectedStackBase == kCoordinateExecutionDefaultFrame);
    REQUIRE(plan.seedSlotBeforeRun);
    REQUIRE(!plan.seedSlotAtHook);
    REQUIRE(plan.timeoutMicros == 120000);
    REQUIRE(plan.instructionBudget == 3000000);
}

void TestUnknownRelativeCandidate() {
    CoordinateExecutionRequest request{};
    request.mode = CoordinateExecutionMode::Emulate;
    request.layout = SyntheticLayout();
    request.candidate.q1 = 0x2000;

    const auto plan = BuildCoordinateExecutionPlan(
        kModuleBase, kModuleSize, kCodeBase, kCodeSize, kSubject, request);
    REQUIRE(plan.valid);
    REQUIRE(plan.x0 == kNormalizedSubject);
    REQUIRE(plan.x1 == kNormalizedSubject);
    REQUIRE(plan.x2 == kNormalizedSubject);
    REQUIRE(plan.expectedStackBase == 0);
    REQUIRE(!plan.seedSlotBeforeRun);
    REQUIRE(plan.timeoutMicros == 45000);
    REQUIRE(plan.instructionBudget == 600000);
}

void TestKnownCandidateRequiresContext() {
    CoordinateExecutionRequest request{};
    request.mode = CoordinateExecutionMode::Emulate;
    request.layout = SyntheticLayout();
    request.candidateKnown = true;
    request.candidate.q1 = 0x2000;
    const auto knownPlan = BuildCoordinateExecutionPlan(
        kModuleBase,
        kModuleSize,
        kCodeBase,
        kCodeSize,
        kSubject,
        request);
    REQUIRE(!knownPlan.valid);
}

void TestAbsoluteCandidate() {
    CoordinateExecutionRequest request{};
    request.mode = CoordinateExecutionMode::Emulate;
    request.layout = SyntheticLayout();
    request.candidate.q1 = 0x3000;
    request.candidate.q2 = UINT64_C(0x0000007030000000);
    request.candidate.q3 = kModuleBase + 0x7000;

    const auto plan = BuildCoordinateExecutionPlan(
        kModuleBase, kModuleSize, kCodeBase, kCodeSize, kSubject, request);
    REQUIRE(plan.valid);
    REQUIRE(plan.entryPc == request.candidate.q2);
    REQUIRE(plan.hookPc == kCodeBase + request.candidate.q1);
    REQUIRE(plan.x0 == kNormalizedSubject);
    REQUIRE(plan.x1 == kNormalizedSubject);
    REQUIRE(plan.x2 == kNormalizedSubject);
    REQUIRE(plan.lr == request.candidate.q3);
    REQUIRE(plan.returnStub == request.candidate.q3);
    REQUIRE(plan.requireReturnStub);
    REQUIRE(plan.verifyReturnStubMagic);
    REQUIRE(!plan.seedSlotBeforeRun);
    REQUIRE(!plan.seedSlotAtHook);

    request.candidate.q3 = kCodeBase + 0x7000;
    REQUIRE(!BuildCoordinateExecutionPlan(
                 kModuleBase,
                 kModuleSize,
                 kCodeBase,
                 kCodeSize,
                 kSubject,
                 request)
                 .valid);
}

void TestSharedRelativeModes() {
    for (const CoordinateExecutionMode mode : {
             CoordinateExecutionMode::Interpret,
             CoordinateExecutionMode::Predecode,
             CoordinateExecutionMode::Jit,
         }) {
        CoordinateExecutionRequest request{};
        request.mode = mode;
        request.layout = SyntheticLayout();
        request.shared.hookOffset = 0x4000;
        request.shared.x0Override = UINT64_C(0xEF00007050000000);

        const auto plan = BuildCoordinateExecutionPlan(
            kModuleBase,
            kModuleSize,
            kCodeBase,
            kCodeSize,
            kSubject,
            request);
        REQUIRE(plan.valid);
        REQUIRE(plan.entryPc == kCodeBase + 0x4000);
        REQUIRE(plan.hookPc == plan.entryPc);
        REQUIRE(plan.x0 == UINT64_C(0x0000007050000000));
        REQUIRE(plan.x1 == kCoordinateExecutionDefaultFrame);
        REQUIRE(plan.x2 == kNormalizedSubject);
        REQUIRE(plan.lr == kCoordinateExecutionStopPc);
        REQUIRE(plan.seedSlotBeforeRun);
        REQUIRE(plan.seedSlotAtHook);
        REQUIRE(plan.timeoutMicros == 20000);
        REQUIRE(plan.instructionBudget == 4000000);
    }
}

void TestSharedAbsoluteEntry() {
    CoordinateExecutionRequest request{};
    request.mode = CoordinateExecutionMode::Interpret;
    request.layout = SyntheticLayout();
    request.shared.hookOffset = 0x5000;
    request.shared.x0Override = UINT64_C(0x0000007050000000);
    request.shared.absoluteEntry = UINT64_C(0x0000007060000000);
    request.shared.returnStub = kModuleBase + 0x6000;

    const auto plan = BuildCoordinateExecutionPlan(
        kModuleBase, kModuleSize, kCodeBase, kCodeSize, kSubject, request);
    REQUIRE(plan.valid);
    REQUIRE(plan.entryPc == request.shared.absoluteEntry);
    REQUIRE(plan.hookPc == kCodeBase + 0x5000);
    REQUIRE(plan.x0 == kNormalizedSubject);
    REQUIRE(plan.x1 == kNormalizedSubject);
    REQUIRE(plan.lr == request.shared.returnStub);
    REQUIRE(plan.returnStub == request.shared.returnStub);
    REQUIRE(ShouldRedirectCoordinateExecutionReturn(
        plan, request.shared.returnStub));
    REQUIRE(ShouldRedirectCoordinateExecutionReturn(
        plan, UINT64_C(0xAB00007000006000)));
    REQUIRE(!ShouldRedirectCoordinateExecutionReturn(
        plan, request.shared.returnStub + 4));
    REQUIRE(!plan.seedSlotBeforeRun);
    REQUIRE(plan.seedSlotAtHook);

    request.shared.returnStub = kModuleBase + kModuleSize;
    REQUIRE(!BuildCoordinateExecutionPlan(
                 kModuleBase,
                 kModuleSize,
                 kCodeBase,
                 kCodeSize,
                 kSubject,
                 request)
                 .valid);
}

void TestInvalidRequests() {
    CoordinateExecutionRequest request{};
    request.mode = static_cast<CoordinateExecutionMode>(0);
    request.layout = SyntheticLayout();
    REQUIRE(!BuildCoordinateExecutionPlan(
                 kModuleBase,
                 kModuleSize,
                 kCodeBase,
                 kCodeSize,
                 kSubject,
                 request)
                 .valid);

    request.mode = CoordinateExecutionMode::Interpret;
    request.layout = {};
    request.shared.hookOffset = 4;
    REQUIRE(!BuildCoordinateExecutionPlan(
                 kModuleBase,
                 kModuleSize,
                 kCodeBase,
                 kCodeSize,
                 kSubject,
                 request)
                 .valid);

    request.layout = SyntheticLayout();
    request.mode = CoordinateExecutionMode::Interpret;
    request.shared.hookOffset = kCodeSize;
    REQUIRE(!BuildCoordinateExecutionPlan(
                 kModuleBase,
                 kModuleSize,
                 kCodeBase,
                 kCodeSize,
                 kSubject,
                 request)
                 .valid);

    request.shared.hookOffset = 4;
    REQUIRE(!BuildCoordinateExecutionPlan(
                 kModuleBase,
                 kModuleSize,
                 kCodeBase,
                 kCodeSize,
                 UINT64_C(0x1000),
                 request)
                 .valid);
}

}  // namespace

int main() {
    TestAbiAndPointers();
    TestSyntheticStackContract();
    TestHookInitializationGate();
    TestSvcContract();
    TestExternalBranchContract();
    TestFakeDescriptorSeekContract();
    TestTaggedMemoryInstructionContract();
    TestExclusiveMonitorContract();
    TestKnownRelativeCandidate();
    TestUnknownRelativeCandidate();
    TestKnownCandidateRequiresContext();
    TestAbsoluteCandidate();
    TestSharedRelativeModes();
    TestSharedAbsoluteEntry();
    TestInvalidRequests();
    return 0;
}
