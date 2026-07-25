#include "game/native/CoordinateExecutionRuntime.h"

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
using lengjing::game::native::CoordinateExecutionRequest;
using lengjing::game::native::CoordinateExecutionStatus;
using lengjing::game::native::ContainsCoordinateExecutionCodeAddress;
using lengjing::game::native::IsCoordinateExecutionPointer;
using lengjing::game::native::NormalizeCoordinateExecutionPointer;
using lengjing::game::native::kCoordinateExecutionDefaultFrame;
using lengjing::game::native::kCoordinateExecutionStopPc;

constexpr std::uint64_t kModuleBase = UINT64_C(0x0000007000000000);
constexpr std::size_t kModuleSize = 0x200000;
constexpr std::uint64_t kSubject = UINT64_C(0xAB00007010000000);
constexpr std::uint64_t kNormalizedSubject = UINT64_C(0x0000007010000000);

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
}

void TestKnownRelativeCandidate() {
    CoordinateExecutionRequest request{};
    request.mode = CoordinateExecutionMode::Emulate;
    request.candidateKnown = true;
    request.candidate.q0 = UINT64_C(0xCD00007020000000);
    request.candidate.q1 = 0x1000;

    const auto plan = BuildCoordinateExecutionPlan(
        kModuleBase, kModuleSize, kSubject, request);
    REQUIRE(plan.valid);
    REQUIRE(plan.entryPc == kModuleBase + 0x1000);
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
    request.candidate.q1 = 0x2000;

    const auto plan = BuildCoordinateExecutionPlan(
        kModuleBase, kModuleSize, kSubject, request);
    REQUIRE(plan.valid);
    REQUIRE(plan.x0 == kNormalizedSubject);
    REQUIRE(plan.x1 == kNormalizedSubject);
    REQUIRE(plan.x2 == kNormalizedSubject);
    REQUIRE(plan.expectedStackBase == 0);
    REQUIRE(!plan.seedSlotBeforeRun);
    REQUIRE(plan.timeoutMicros == 45000);
    REQUIRE(plan.instructionBudget == 600000);

    request.candidateKnown = true;
    REQUIRE(!BuildCoordinateExecutionPlan(
                 kModuleBase, kModuleSize, kSubject, request)
                 .valid);
}

void TestAbsoluteCandidate() {
    CoordinateExecutionRequest request{};
    request.mode = CoordinateExecutionMode::Emulate;
    request.candidate.q1 = 0x3000;
    request.candidate.q2 = UINT64_C(0x0000007030000000);
    request.candidate.q3 = UINT64_C(0x0000007040000000);

    const auto plan = BuildCoordinateExecutionPlan(
        kModuleBase, kModuleSize, kSubject, request);
    REQUIRE(plan.valid);
    REQUIRE(plan.entryPc == request.candidate.q2);
    REQUIRE(plan.hookPc == kModuleBase + request.candidate.q1);
    REQUIRE(plan.x0 == kNormalizedSubject);
    REQUIRE(plan.x1 == kNormalizedSubject);
    REQUIRE(plan.x2 == kNormalizedSubject);
    REQUIRE(plan.lr == request.candidate.q3);
    REQUIRE(plan.returnStub == request.candidate.q3);
    REQUIRE(plan.requireReturnStub);
    REQUIRE(plan.verifyReturnStubMagic);
    REQUIRE(!plan.seedSlotBeforeRun);
    REQUIRE(!plan.seedSlotAtHook);
}

void TestSharedRelativeModes() {
    for (const CoordinateExecutionMode mode : {
             CoordinateExecutionMode::Interpret,
             CoordinateExecutionMode::Predecode,
             CoordinateExecutionMode::Jit,
         }) {
        CoordinateExecutionRequest request{};
        request.mode = mode;
        request.shared.hookOffset = 0x4000;
        request.shared.x0Override = UINT64_C(0xEF00007050000000);

        const auto plan = BuildCoordinateExecutionPlan(
            kModuleBase, kModuleSize, kSubject, request);
        REQUIRE(plan.valid);
        REQUIRE(plan.entryPc == kModuleBase + 0x4000);
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
    request.shared.hookOffset = 0x5000;
    request.shared.absoluteEntry = UINT64_C(0x0000007060000000);
    request.shared.returnStub = kModuleBase + 0x6000;

    const auto plan = BuildCoordinateExecutionPlan(
        kModuleBase, kModuleSize, kSubject, request);
    REQUIRE(plan.valid);
    REQUIRE(plan.entryPc == request.shared.absoluteEntry);
    REQUIRE(plan.hookPc == kModuleBase + 0x5000);
    REQUIRE(plan.x1 == kNormalizedSubject);
    REQUIRE(plan.lr == request.shared.returnStub);
    REQUIRE(plan.returnStub == request.shared.returnStub);
    REQUIRE(!plan.seedSlotBeforeRun);
    REQUIRE(plan.seedSlotAtHook);

    request.shared.returnStub = kModuleBase + kModuleSize;
    REQUIRE(!BuildCoordinateExecutionPlan(
                 kModuleBase, kModuleSize, kSubject, request)
                 .valid);
}

void TestInvalidRequests() {
    CoordinateExecutionRequest request{};
    request.mode = static_cast<CoordinateExecutionMode>(0);
    REQUIRE(!BuildCoordinateExecutionPlan(
                 kModuleBase, kModuleSize, kSubject, request)
                 .valid);

    request.mode = CoordinateExecutionMode::Interpret;
    request.shared.hookOffset = kModuleSize;
    REQUIRE(!BuildCoordinateExecutionPlan(
                 kModuleBase, kModuleSize, kSubject, request)
                 .valid);

    request.shared.hookOffset = 4;
    REQUIRE(!BuildCoordinateExecutionPlan(
                 kModuleBase, kModuleSize, UINT64_C(0x1000), request)
                 .valid);
}

}  // namespace

int main() {
    TestAbiAndPointers();
    TestKnownRelativeCandidate();
    TestUnknownRelativeCandidate();
    TestAbsoluteCandidate();
    TestSharedRelativeModes();
    TestSharedAbsoluteEntry();
    TestInvalidRequests();
    return 0;
}
