#if 0

#include "game/native/CoordinateDecrypt3Runtime.h"
#include "test_support.h"

#include <array>
#include <cstdint>

namespace {

using lengjing::game::native::coordinate_decrypt3_abi::NormalizePointer;
using lengjing::game::native::coordinate_decrypt3_abi::PollPayload;
using lengjing::game::native::coordinate_decrypt3_abi::SelectSampleMode;
using lengjing::game::native::coordinate_decrypt3_abi::
    SelectSnapshotCandidate;

constexpr std::uintptr_t kInstruction = UINT64_C(0x70001000);
constexpr std::uintptr_t kCandidateA = UINT64_C(0x72000000);
constexpr std::uintptr_t kCandidateB = UINT64_C(0x73000000);
constexpr std::uintptr_t kCandidateC = UINT64_C(0x74000000);

PollPayload ValidSnapshot() {
    PollPayload snapshot{};
    snapshot.instructionAddress = kInstruction;
    snapshot.primaryCandidate =
        UINT64_C(0xab00000072000000);
    snapshot.sequence = 9;
    snapshot.hit = 1;
    snapshot.state = 1;
    return snapshot;
}

void TestPointerNormalization() {
    REQUIRE(
        NormalizePointer(UINT64_C(0xab123456789abcde)) ==
        UINT64_C(0x00123456789abcde));
    REQUIRE(NormalizePointer(0) == 0);
}

void TestSnapshotSelection() {
    PollPayload snapshot = ValidSnapshot();
    std::uintptr_t candidate = 0;
    REQUIRE(SelectSnapshotCandidate(
        snapshot, kInstruction, 8, candidate));
    REQUIRE(candidate == kCandidateA);

    snapshot.primaryCandidate = 0;
    snapshot.registers[23] =
        UINT64_C(0xcd00000073000000);
    REQUIRE(SelectSnapshotCandidate(
        snapshot, kInstruction, 8, candidate));
    REQUIRE(candidate == kCandidateB);

    snapshot.hit = 0;
    REQUIRE(!SelectSnapshotCandidate(
        snapshot, kInstruction, 8, candidate));
    snapshot = ValidSnapshot();
    snapshot.state = 2;
    REQUIRE(!SelectSnapshotCandidate(
        snapshot, kInstruction, 8, candidate));
    snapshot = ValidSnapshot();
    snapshot.sequence = 8;
    REQUIRE(!SelectSnapshotCandidate(
        snapshot, kInstruction, 8, candidate));
    snapshot = ValidSnapshot();
    snapshot.instructionAddress += 4;
    REQUIRE(!SelectSnapshotCandidate(
        snapshot, kInstruction, 8, candidate));
}

void TestPhysicalOrderModeSelection() {
    const std::array<std::uintptr_t, 10> samples{
        kCandidateB,
        kCandidateA,
        kCandidateA,
        kCandidateB,
        kCandidateC,
        kCandidateA,
        kCandidateA,
        kCandidateA,
        kCandidateB,
        kCandidateA,
    };
    REQUIRE(SelectSampleMode(samples, 10) == kCandidateA);
    REQUIRE(SelectSampleMode(samples, 4) == kCandidateB);
    REQUIRE(SelectSampleMode(samples, 1) == kCandidateB);
    REQUIRE(SelectSampleMode(samples, 0) == 0);
    REQUIRE(SelectSampleMode(samples, 11) == 0);
}

}  // namespace

void RunCoordinateDecrypt3AbiTests() {
    TestPointerNormalization();
    TestSnapshotSelection();
    TestPhysicalOrderModeSelection();
}

#endif
