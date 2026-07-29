#include "game/native/ExecutionSnapshotTransport.h"
#include "test_support.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using lengjing::game::native::ExecutionSnapshot;
using lengjing::game::native::ExecutionSnapshotPollResult;
using lengjing::game::native::ExecutionSnapshotTransport;
namespace snapshot_abi =
    lengjing::game::native::execution_snapshot_abi;

struct SubmitFixture {
    std::vector<std::uint32_t> operations;
    snapshot_abi::PollPayload nextSnapshot{};
    int disarmResult = 0;
    int armResult = 0;
    int pollResult = 0;
    snapshot_abi::ArmPayload arm{};
};

int Submit(void* context,
           std::uint32_t operation,
           void* payload) noexcept {
    auto& fixture = *static_cast<SubmitFixture*>(context);
    fixture.operations.push_back(operation);
    switch (operation) {
        case snapshot_abi::kDisarmOperation:
            return fixture.disarmResult;
        case snapshot_abi::kArmOperation:
            fixture.arm =
                *static_cast<snapshot_abi::ArmPayload*>(
                    payload);
            return fixture.armResult;
        case snapshot_abi::kPollOperation:
            *static_cast<snapshot_abi::PollPayload*>(
                payload) = fixture.nextSnapshot;
            return fixture.pollResult;
        default:
            return -1;
    }
}

void TestAbiLayoutAndPointerNormalization() {
    REQUIRE(sizeof(snapshot_abi::Envelope) == 0x10);
    REQUIRE(offsetof(snapshot_abi::Envelope, payload) == 0x8);
    REQUIRE(sizeof(snapshot_abi::ArmPayload) == 0x28);
    REQUIRE(sizeof(snapshot_abi::PollPayload) == 0x160);
    REQUIRE(offsetof(
                snapshot_abi::PollPayload,
                registers[23]) == 0xb8);
    REQUIRE(offsetof(
                snapshot_abi::PollPayload,
                primaryCandidate) == 0x130);
    REQUIRE(offsetof(
                snapshot_abi::PollPayload,
                sequence) == 0x138);
    REQUIRE(offsetof(
                snapshot_abi::PollPayload,
                hit) == 0x144);
    REQUIRE(offsetof(
                snapshot_abi::PollPayload,
                state) == 0x148);
    REQUIRE(snapshot_abi::kRequestCount == 0x9400);
    REQUIRE(snapshot_abi::NormalizePointer(
                UINT64_C(0xab00123456789abc)) ==
            UINT64_C(0x00123456789abc));
}

void TestStartPollAndStopProtocol() {
    SubmitFixture fixture{};
    ExecutionSnapshotTransport transport(Submit, &fixture);
    constexpr pid_t kProcessId = 4321;
    constexpr std::uintptr_t kInstruction = 0x12345000;

    REQUIRE(transport.Start(kProcessId, kInstruction));
    REQUIRE(transport.IsActive());
    REQUIRE(fixture.operations.size() == 1);
    REQUIRE(fixture.operations[0] ==
            snapshot_abi::kArmOperation);
    REQUIRE(fixture.arm.processId == kProcessId);
    REQUIRE(fixture.arm.instructionAddress == kInstruction);
    REQUIRE(fixture.arm.literalAddress == 0);
    REQUIRE(fixture.arm.protectedPage == 0);
    REQUIRE(fixture.arm.instruction == 0);
    REQUIRE(fixture.arm.literalSize == 0);

    fixture.nextSnapshot.instructionAddress = kInstruction;
    fixture.nextSnapshot.primaryCandidate =
        UINT64_C(0xaa00712345678000);
    fixture.nextSnapshot.registers[23] =
        UINT64_C(0xbb00700000000000);
    fixture.nextSnapshot.sequence = 7;
    fixture.nextSnapshot.processId = kProcessId;
    fixture.nextSnapshot.hit = 1;
    fixture.nextSnapshot.state = 1;

    ExecutionSnapshot snapshot{};
    REQUIRE(transport.Poll(snapshot) ==
            ExecutionSnapshotPollResult::Accepted);
    REQUIRE(snapshot.candidate ==
            UINT64_C(0x00712345678000));
    REQUIRE(snapshot.primaryCandidate ==
            UINT64_C(0x00712345678000));
    REQUIRE(snapshot.fallbackCandidate ==
            UINT64_C(0x00700000000000));
    REQUIRE(!snapshot.usedFallback);
    REQUIRE(snapshot.sequence == 7);
    REQUIRE(snapshot.processId == kProcessId);
    REQUIRE(snapshot.state == 1);
    REQUIRE(fixture.operations.back() ==
            snapshot_abi::kPollOperation);

    const auto probe = transport.Probe();
    REQUIRE(probe.active);
    REQUIRE(probe.sequence == 7);
    REQUIRE(probe.pollCount == 1);
    REQUIRE(probe.acceptedCount == 1);

    REQUIRE(transport.Stop());
    REQUIRE(!transport.IsActive());
    REQUIRE(fixture.operations.back() ==
            snapshot_abi::kDisarmOperation);
}

void TestFallbackAndSnapshotValidation() {
    SubmitFixture fixture{};
    ExecutionSnapshotTransport transport(Submit, &fixture);
    constexpr pid_t kProcessId = 99;
    constexpr std::uintptr_t kInstruction = 0x4000;
    REQUIRE(transport.Start(kProcessId, kInstruction));

    fixture.nextSnapshot.instructionAddress = kInstruction;
    fixture.nextSnapshot.registers[23] =
        UINT64_C(0xcd00765432100000);
    fixture.nextSnapshot.sequence = 1;
    fixture.nextSnapshot.processId = kProcessId;
    fixture.nextSnapshot.hit = 1;
    fixture.nextSnapshot.state = 1;

    ExecutionSnapshot snapshot{};
    REQUIRE(transport.Poll(snapshot) ==
            ExecutionSnapshotPollResult::Accepted);
    REQUIRE(snapshot.candidate ==
            UINT64_C(0x00765432100000));
    REQUIRE(snapshot.usedFallback);

    REQUIRE(transport.Poll(snapshot) ==
            ExecutionSnapshotPollResult::Retry);
    REQUIRE(transport.Probe().sequence == 1);

    fixture.nextSnapshot.sequence = 2;
    fixture.nextSnapshot.hit = 0;
    REQUIRE(transport.Poll(snapshot) ==
            ExecutionSnapshotPollResult::Retry);
    REQUIRE(transport.Probe().sequence == 1);

    fixture.nextSnapshot.sequence = 3;
    fixture.nextSnapshot.hit = 1;
    fixture.nextSnapshot.state = 2;
    REQUIRE(transport.Poll(snapshot) ==
            ExecutionSnapshotPollResult::Reconfigure);
    REQUIRE(transport.Probe().sequence == 1);
    REQUIRE(transport.Probe().needsReconfigure);
    REQUIRE(!transport.IsActive());
    REQUIRE(fixture.operations.back() ==
            snapshot_abi::kDisarmOperation);

    REQUIRE(transport.Start(kProcessId, kInstruction));
    REQUIRE(transport.Probe().sequence == 1);
    fixture.nextSnapshot.sequence = 4;
    fixture.nextSnapshot.state = 1;
    fixture.nextSnapshot.instructionAddress =
        kInstruction + 4;
    REQUIRE(transport.Poll(snapshot) ==
            ExecutionSnapshotPollResult::Retry);
    REQUIRE(transport.Probe().sequence == 1);

    fixture.nextSnapshot.sequence = 5;
    fixture.nextSnapshot.instructionAddress = kInstruction;
    fixture.nextSnapshot.registers[23] = 0;
    REQUIRE(transport.Poll(snapshot) ==
            ExecutionSnapshotPollResult::Retry);
    REQUIRE(transport.Probe().sequence == 5);
}

void TestFailureAndRearmBehavior() {
    SubmitFixture fixture{};
    fixture.armResult = -5;
    ExecutionSnapshotTransport transport(Submit, &fixture);
    REQUIRE(!transport.Start(77, 0x8000));
    REQUIRE(!transport.IsActive());
    REQUIRE(transport.Probe().error ==
            lengjing::game::native::
                ExecutionSnapshotTransportError::ArmFailed);
    REQUIRE(transport.Probe().needsReconfigure);

    fixture = {};
    REQUIRE(transport.Start(77, 0x8000));
    fixture.nextSnapshot.instructionAddress = 0x8000;
    fixture.nextSnapshot.primaryCandidate = 0x9000;
    fixture.nextSnapshot.sequence = 12;
    fixture.nextSnapshot.processId = 77;
    fixture.nextSnapshot.hit = 1;
    fixture.nextSnapshot.state = 1;
    ExecutionSnapshot snapshot{};
    REQUIRE(transport.Poll(snapshot) ==
            ExecutionSnapshotPollResult::Accepted);
    REQUIRE(transport.Rearm());
    REQUIRE(fixture.operations[
                fixture.operations.size() - 2] ==
            snapshot_abi::kDisarmOperation);
    REQUIRE(fixture.operations.back() ==
            snapshot_abi::kArmOperation);
    REQUIRE(transport.Probe().sequence == 12);

    fixture.pollResult = -9;
    REQUIRE(transport.Poll(snapshot) ==
            ExecutionSnapshotPollResult::Reconfigure);
    REQUIRE(transport.Probe().error ==
            lengjing::game::native::
                ExecutionSnapshotTransportError::PollFailed);
    REQUIRE(transport.Probe().systemError == -9);
    REQUIRE(transport.Probe().needsReconfigure);
    REQUIRE(!transport.IsActive());
    REQUIRE(fixture.operations.back() ==
            snapshot_abi::kDisarmOperation);

    transport.Reset();
    REQUIRE(transport.Probe().sequence == 0);
    REQUIRE(transport.Probe().pollCount == 0);
}

void TestDisarmFailureIsRetried() {
    SubmitFixture fixture{};
    ExecutionSnapshotTransport transport(Submit, &fixture);
    REQUIRE(transport.Start(88, 0x9000));

    fixture.disarmResult = -7;
    const std::size_t beforeFirstStop = fixture.operations.size();
    REQUIRE(!transport.Stop());
    REQUIRE(transport.IsActive());
    REQUIRE(transport.Probe().cleanupPending);
    REQUIRE(transport.Probe().needsReconfigure);
    REQUIRE(fixture.operations.size() == beforeFirstStop + 1);
    REQUIRE(fixture.operations.back() ==
            snapshot_abi::kDisarmOperation);

    fixture.disarmResult = 0;
    REQUIRE(transport.Stop());
    REQUIRE(!transport.IsActive());
    REQUIRE(!transport.Probe().cleanupPending);
    REQUIRE(!transport.Probe().needsReconfigure);
    REQUIRE(fixture.operations.size() == beforeFirstStop + 2);
    REQUIRE(fixture.operations.back() ==
            snapshot_abi::kDisarmOperation);
}

void TestResetRetainsPendingCleanup() {
    SubmitFixture fixture{};
    ExecutionSnapshotTransport transport(Submit, &fixture);
    REQUIRE(transport.Start(89, 0xa000));

    fixture.disarmResult = -8;
    const std::size_t beforeReset = fixture.operations.size();
    transport.Reset();
    REQUIRE(transport.IsActive());
    REQUIRE(transport.Probe().cleanupPending);
    REQUIRE(transport.Probe().needsReconfigure);
    REQUIRE(fixture.operations.size() == beforeReset + 1);
    REQUIRE(fixture.operations.back() ==
            snapshot_abi::kDisarmOperation);

    fixture.disarmResult = 0;
    transport.Reset();
    REQUIRE(!transport.IsActive());
    REQUIRE(!transport.Probe().cleanupPending);
    REQUIRE(!transport.Probe().needsReconfigure);
    REQUIRE(transport.Probe().sequence == 0);
    REQUIRE(fixture.operations.size() == beforeReset + 2);
    REQUIRE(fixture.operations.back() ==
            snapshot_abi::kDisarmOperation);
}

void TestInvalidStartInput() {
    SubmitFixture fixture{};
    ExecutionSnapshotTransport transport(Submit, &fixture);
    REQUIRE(!transport.Start(0, 0x1000));
    REQUIRE(!transport.Start(1, 0));
    REQUIRE(!transport.Start(1, 0x1002));
    REQUIRE(fixture.operations.empty());
}

void TestFallbackPolicy() {
    using lengjing::game::native::ExecutionSnapshotFallbackPolicy;

    ExecutionSnapshotFallbackPolicy policy;
    REQUIRE(!policy.ShouldFallback());
    policy.Observe(ExecutionSnapshotPollResult::Retry);
    REQUIRE(policy.ReconfigureFailures() == 0);
    policy.Observe(ExecutionSnapshotPollResult::Reconfigure);
    policy.Observe(ExecutionSnapshotPollResult::Reconfigure);
    REQUIRE(policy.ReconfigureFailures() == 2);
    REQUIRE(!policy.ShouldFallback());
    policy.Observe(ExecutionSnapshotPollResult::Accepted);
    REQUIRE(policy.ReconfigureFailures() == 0);

    for (std::uint32_t index = 0;
         index <
             ExecutionSnapshotFallbackPolicy::kReconfigureLimit + 2;
         ++index) {
        policy.Observe(ExecutionSnapshotPollResult::Reconfigure);
    }
    REQUIRE(policy.ReconfigureFailures() ==
            ExecutionSnapshotFallbackPolicy::kReconfigureLimit);
    REQUIRE(policy.ShouldFallback());
    policy.Reset();
    REQUIRE(policy.ReconfigureFailures() == 0);
    REQUIRE(!policy.ShouldFallback());
}

}  // namespace

void RunExecutionSnapshotTransportTests() {
    TestAbiLayoutAndPointerNormalization();
    TestStartPollAndStopProtocol();
    TestFallbackAndSnapshotValidation();
    TestFailureAndRearmBehavior();
    TestDisarmFailureIsRetried();
    TestResetRetainsPendingCleanup();
    TestInvalidStartInput();
    TestFallbackPolicy();
}
