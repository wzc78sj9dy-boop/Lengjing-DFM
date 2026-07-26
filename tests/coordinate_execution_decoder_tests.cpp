#include "game/native/CoordinateExecutionDecoder.h"

#include "game/native/MemoryTransport.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#define REQUIRE(condition)                                                    \
    do {                                                                      \
        if (!(condition)) {                                                   \
            throw std::runtime_error(                                         \
                std::string(__FILE__) + ":" + std::to_string(__LINE__) +     \
                ": requirement failed: " #condition);                       \
        }                                                                     \
    } while (false)

namespace game = lengjing::game::native;

namespace lengjing::game::native {

struct CoordinateExecutionRuntime::Impl {};

CoordinateExecutionRuntime::CoordinateExecutionRuntime()
    : impl_(std::make_unique<Impl>()) {}

CoordinateExecutionRuntime::~CoordinateExecutionRuntime() = default;

CoordinateExecutionResult CoordinateExecutionRuntime::Execute(
    MemoryTransport&,
    std::uintptr_t,
    std::size_t,
    std::uintptr_t,
    std::size_t,
    std::uintptr_t,
    const ProcessExecutionContext&,
    const CoordinateExecutionRequest&) noexcept {
    CoordinateExecutionResult result{};
    result.status = CoordinateExecutionStatus::BackendUnavailable;
    return result;
}

CoordinateExecutionRuntimeProbe CoordinateExecutionRuntime::Probe()
    const noexcept {
    return {};
}

void CoordinateExecutionRuntime::Reset() noexcept {}

bool MemoryTransport::Read(
    std::uintptr_t,
    void*,
    std::size_t) {
    return false;
}

bool MemoryTransport::ReadCoordinateMemory(
    std::uintptr_t,
    void*,
    std::size_t,
    game::CoordinateReadDiagnostic&) {
    return false;
}

}  // namespace lengjing::game::native

namespace {

constexpr std::uint64_t kModuleBase = UINT64_C(0x7000000000);
constexpr std::size_t kModuleSize = 0x0F000000;
constexpr std::uint64_t kCodeBase = UINT64_C(0x7562560000);
constexpr std::size_t kCodeSize = 0x0E2000;
constexpr std::uint64_t kRelativeEntry = UINT64_C(0x4000);
constexpr std::uint64_t kSubject = UINT64_C(0x7001000000);
constexpr std::int32_t kProcessId = 3210;
constexpr std::size_t kCandidateCount = 70;

constexpr std::uint32_t EncodeLdrLiteral(std::int32_t wordDisplacement,
                                         unsigned targetRegister) {
    return UINT32_C(0x58000000) |
        ((static_cast<std::uint32_t>(wordDisplacement) & 0x7FFFFU) << 5U) |
        (targetRegister & 0x1FU);
}

constexpr std::uint32_t EncodeBr(unsigned targetRegister) {
    return UINT32_C(0xD61F0000) | ((targetRegister & 0x1FU) << 5U);
}

constexpr game::CoordinateExecutionLayout SyntheticLayout() {
    game::CoordinateExecutionLayout layout{};
    layout.discovery.rootOffset = UINT64_C(0x234000);
    layout.discovery.pointerOffset = UINT64_C(0x18);
    layout.discovery.entryOffset = UINT64_C(0xB8);
    layout.discovery.returnStubMagic =
        (static_cast<std::uint64_t>(EncodeBr(17)) << 32U) |
        EncodeLdrLiteral(3, 17);
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

constexpr std::uint64_t CandidateQ0(std::size_t index) {
    return UINT64_C(0x6002000000) + index * UINT64_C(0x1000);
}

constexpr std::uint64_t CandidateQ2(std::size_t index) {
    return UINT64_C(0x6001000000) + index * UINT64_C(0x1000);
}

class SparseMemory {
public:
    template <typename T>
    void Write(std::uint64_t address, const T& value) {
        const auto* bytes = reinterpret_cast<const std::byte*>(&value);
        for (std::size_t index = 0; index < sizeof(T); ++index) {
            bytes_[address + index] = bytes[index];
        }
    }

    bool Read(std::uint64_t address, void* output, std::size_t size) {
        auto* bytes = static_cast<std::byte*>(output);
        std::memset(bytes, 0, size);
        bool any = false;
        for (auto item = bytes_.lower_bound(address);
             item != bytes_.end() && item->first - address < size;
             ++item) {
            bytes[item->first - address] = item->second;
            any = true;
        }
        return any;
    }

    game::CoordinateExecutionReadCallback Callback() {
        return [this](std::uint64_t address,
                      void* output,
                      std::size_t size) {
            return Read(address, output, size);
        };
    }

private:
    std::map<std::uint64_t, std::byte> bytes_;
};

void InstallCandidate(SparseMemory& memory,
                      std::uint64_t ldrPc,
                      std::uint64_t thunk,
                      std::uint64_t q0) {
    constexpr auto kLayout = SyntheticLayout();
    memory.Write(ldrPc - 4, UINT32_C(0xD503201F));
    memory.Write(ldrPc, kLayout.discovery.returnStubMagic);
    memory.Write(ldrPc + 12, thunk);

    memory.Write(thunk, EncodeLdrLiteral(4, 0));
    memory.Write(thunk + 4, EncodeLdrLiteral(5, 1));
    memory.Write(thunk + 8, EncodeBr(2));
    memory.Write(thunk + 16, q0);
    memory.Write(thunk + 24, kCodeBase + kRelativeEntry);
}

std::uint64_t CandidateQ3(std::size_t index) {
    constexpr auto kLayout = SyntheticLayout();
    return kModuleBase + kLayout.discovery.rootOffset + UINT64_C(0x1000) +
        index * UINT64_C(0x40);
}

SparseMemory BuildCandidateMemory() {
    SparseMemory memory;
    constexpr auto kLayout = SyntheticLayout();
    const std::uint64_t anchor =
        kModuleBase + kLayout.discovery.rootOffset;
    constexpr std::uint64_t kRoot = UINT64_C(0x6000003000);
    memory.Write(anchor + kLayout.discovery.pointerOffset, kRoot);
    memory.Write(
        kRoot + kLayout.discovery.entryOffset,
        kCodeBase + kRelativeEntry);
    for (std::size_t index = 0; index < kCandidateCount; ++index) {
        InstallCandidate(
            memory,
            CandidateQ3(index) + 4,
            CandidateQ2(index),
            CandidateQ0(index));
    }
    return memory;
}

game::ProcessExecutionContext ExecutionContext(std::uint64_t generation) {
    game::ProcessExecutionContext context{};
    context.tpidrEl0 = UINT64_C(0x7003000000);
    context.threadId = 77;
    context.generation = generation;
    context.pacgaOracle.available = true;
    context.pacgaOracle.data = 1;
    context.pacgaOracle.modifier = 2;
    context.pacgaOracle.result = 3;
    return context;
}

game::CoordinateExecutionResult Failure(
    game::CoordinateExecutionStatus status =
        game::CoordinateExecutionStatus::EvidenceFailure) {
    game::CoordinateExecutionResult result{};
    result.status = status;
    return result;
}

game::CoordinateExecutionResult Success(float marker = 1.0F) {
    game::CoordinateExecutionResult result{};
    result.ok = 1;
    result.status = game::CoordinateExecutionStatus::Success;
    result.position = {marker, marker + 1.0F, marker + 2.0F};
    result.object = UINT64_C(0x6005000000);
    return result;
}

struct Harness {
    std::vector<game::CoordinateExecutionRequest> requests;
    std::function<game::CoordinateExecutionResult(
        const game::CoordinateExecutionRequest&)> responder;
    game::CoordinateExecutionRuntimeProbe runtimeProbe{};
    std::uint64_t now = 0;
    std::size_t resetCount = 0;

    game::CoordinateExecutionDecoderHooks Hooks() {
        game::CoordinateExecutionDecoderHooks hooks{};
        hooks.execute = [this](
                            std::uintptr_t subject,
                            const game::CoordinateExecutionRequest& request) {
            requests.push_back(request);
            runtimeProbe.plan = game::BuildCoordinateExecutionPlan(
                kModuleBase,
                kModuleSize,
                kCodeBase,
                kCodeSize,
                subject,
                request);
            const game::CoordinateExecutionResult result = responder
                ? responder(request)
                : Failure();
            runtimeProbe.status = result.status;
            return result;
        };
        hooks.probe = [this] { return runtimeProbe; };
        hooks.reset = [this] {
            ++resetCount;
            runtimeProbe = {};
        };
        hooks.nowMilliseconds = [this] { return now; };
        return hooks;
    }
};

bool Refresh(game::CoordinateExecutionDecoder& decoder,
             SparseMemory& memory,
             const game::ProcessExecutionContext& context,
             std::int32_t processId = kProcessId,
             std::size_t moduleSize = kModuleSize,
             std::uint64_t codeBase = kCodeBase,
             std::size_t codeSize = kCodeSize) {
    return decoder.Refresh(
        memory.Callback(),
        processId,
        kModuleBase,
        moduleSize,
        codeBase,
        codeSize,
        context);
}

void TestUnknownLimitAndPersistentCursor() {
    SparseMemory memory = BuildCandidateMemory();
    Harness harness;
    game::CoordinateExecutionDecoder decoder(harness.Hooks());
    const game::ProcessExecutionContext context = ExecutionContext(1);
    REQUIRE(decoder.Configure(
        game::CoordinateExecutionMode::Emulate, SyntheticLayout()));
    REQUIRE(Refresh(decoder, memory, context));
    REQUIRE(decoder.Probe().candidateCount == kCandidateCount);

    game::CoordinateExecutionPosition position{};
    REQUIRE(!decoder.Decode(kSubject, position));
    REQUIRE(
        harness.requests.size() ==
        game::kCoordinateExecutionAttemptsPerDecode);
    auto probe = decoder.Probe();
    REQUIRE(probe.cursor == 64);
    REQUIRE(probe.candidateLimitReached);
    REQUIRE(!probe.traversalTimeLimitReached);
    REQUIRE(!probe.knownCandidate);
    for (const auto& request : harness.requests) {
        REQUIRE(request.mode == game::CoordinateExecutionMode::Emulate);
        REQUIRE(request.layout == SyntheticLayout());
        REQUIRE(!request.candidateKnown);
    }
    const auto unknownPlan = game::BuildCoordinateExecutionPlan(
        kModuleBase,
        kModuleSize,
        kCodeBase,
        kCodeSize,
        kSubject,
        harness.requests.front());
    REQUIRE(unknownPlan.valid);
    REQUIRE(unknownPlan.hookPc == kCodeBase + kRelativeEntry);
    REQUIRE(unknownPlan.timeoutMicros == 45000);
    REQUIRE(unknownPlan.instructionBudget == 600000);

    REQUIRE(Refresh(decoder, memory, context));
    REQUIRE(decoder.Probe().cursor == 64);
    harness.requests.clear();
    REQUIRE(!decoder.Decode(kSubject, position));
    REQUIRE(harness.requests.size() == kCandidateCount - 64);
    REQUIRE(decoder.Probe().cursor == 0);
    REQUIRE(decoder.Probe().status ==
            game::CoordinateExecutionStatus::EvidenceFailure);
}

void TestTraversalTimeLimitBetweenCandidates() {
    SparseMemory memory = BuildCandidateMemory();
    Harness harness;
    harness.responder = [&harness](const auto&) {
        harness.now = game::kCoordinateExecutionTraversalLimitMs;
        return Failure();
    };
    game::CoordinateExecutionDecoder decoder(harness.Hooks());
    REQUIRE(decoder.Configure(
        game::CoordinateExecutionMode::Emulate, SyntheticLayout()));
    REQUIRE(Refresh(decoder, memory, ExecutionContext(1)));

    game::CoordinateExecutionPosition position{};
    REQUIRE(!decoder.Decode(kSubject, position));
    REQUIRE(harness.requests.size() == 1);
    const auto probe = decoder.Probe();
    REQUIRE(probe.cursor == 1);
    REQUIRE(probe.traversalTimeLimitReached);
    REQUIRE(!probe.candidateLimitReached);
}

void TestKnownCandidateIsExclusive() {
    SparseMemory memory = BuildCandidateMemory();
    Harness harness;
    harness.responder = [](const auto& request) {
        return !request.candidateKnown &&
                request.candidate.q0 == CandidateQ0(2)
            ? Success(10.0F)
            : Failure();
    };
    game::CoordinateExecutionDecoder decoder(harness.Hooks());
    const game::ProcessExecutionContext context = ExecutionContext(1);
    REQUIRE(decoder.Configure(
        game::CoordinateExecutionMode::Emulate, SyntheticLayout()));
    REQUIRE(Refresh(decoder, memory, context));

    game::CoordinateExecutionPosition position{};
    REQUIRE(decoder.Decode(kSubject, position));
    REQUIRE(harness.requests.size() == 3);
    REQUIRE(position.x == 10.0F);
    REQUIRE(decoder.Probe().knownCandidate);
    REQUIRE(decoder.Probe().cachedCandidate.q0 == CandidateQ0(2));

    REQUIRE(Refresh(decoder, memory, context));
    REQUIRE(decoder.Probe().knownCandidate);
    harness.requests.clear();
    harness.responder = [](const auto&) { return Failure(); };
    REQUIRE(!decoder.Decode(kSubject, position));
    REQUIRE(harness.requests.size() == 1);
    REQUIRE(harness.requests[0].candidateKnown);
    REQUIRE(harness.requests[0].candidate.q0 == CandidateQ0(2));
    REQUIRE(decoder.Probe().knownCandidate);
    REQUIRE(decoder.Probe().status ==
            game::CoordinateExecutionStatus::EvidenceFailure);

    const auto knownPlan = game::BuildCoordinateExecutionPlan(
        kModuleBase,
        kModuleSize,
        kCodeBase,
        kCodeSize,
        kSubject,
        harness.requests[0]);
    REQUIRE(knownPlan.valid);
    REQUIRE(knownPlan.timeoutMicros == 120000);
    REQUIRE(knownPlan.instructionBudget == 3000000);

    const std::size_t contextResetsBefore = harness.resetCount;
    REQUIRE(Refresh(decoder, memory, ExecutionContext(2)));
    REQUIRE(decoder.Probe().knownCandidate);
    REQUIRE(decoder.Probe().cursor == 0);
    REQUIRE(decoder.Probe().candidateCount == kCandidateCount);
    REQUIRE(harness.resetCount > contextResetsBefore);
}

void TestSharedEntryMappingAndInvalidation() {
    SparseMemory memory = BuildCandidateMemory();
    Harness harness;
    harness.responder = [](const auto&) { return Success(); };
    game::CoordinateExecutionDecoder decoder(harness.Hooks());
    const game::ProcessExecutionContext context = ExecutionContext(5);

    for (const auto mode : {
             game::CoordinateExecutionMode::Interpret,
             game::CoordinateExecutionMode::Predecode,
             game::CoordinateExecutionMode::Jit,
         }) {
        REQUIRE(decoder.Configure(mode, SyntheticLayout()));
        REQUIRE(!decoder.Probe().knownCandidate);
        REQUIRE(Refresh(decoder, memory, context));
        harness.requests.clear();
        game::CoordinateExecutionPosition position{};
        REQUIRE(decoder.Decode(kSubject, position));
        REQUIRE(harness.requests.size() == 1);
        const auto& request = harness.requests.front();
        REQUIRE(request.mode == mode);
        REQUIRE(request.layout == SyntheticLayout());
        REQUIRE(request.shared.hookOffset == request.candidate.q1);
        REQUIRE(request.shared.x0Override == request.candidate.q0);
        REQUIRE(request.shared.absoluteEntry == request.candidate.q2);
        REQUIRE(request.shared.returnStub == request.candidate.q3);
        REQUIRE(request.shared.hookOffset == kRelativeEntry);
        REQUIRE(request.shared.x0Override == CandidateQ0(0));
        REQUIRE(request.shared.absoluteEntry == CandidateQ2(0));
        REQUIRE(request.shared.returnStub == CandidateQ3(0));
        const auto plan = game::BuildCoordinateExecutionPlan(
            kModuleBase,
            kModuleSize,
            kCodeBase,
            kCodeSize,
            kSubject,
            request);
        REQUIRE(plan.valid);
        REQUIRE(plan.hookPc == kCodeBase + kRelativeEntry);
        REQUIRE(!decoder.Probe().knownCandidate);
    }

    REQUIRE(!decoder.Probe().knownCandidate);
    const std::size_t resetsBefore = harness.resetCount;
    REQUIRE(Refresh(decoder, memory, context, kProcessId + 1));
    REQUIRE(!decoder.Probe().knownCandidate);
    REQUIRE(decoder.Probe().candidateCount == kCandidateCount);
    REQUIRE(harness.resetCount > resetsBefore);

    harness.responder = [](const auto&) { return Success(); };
    game::CoordinateExecutionPosition position{};
    REQUIRE(decoder.Decode(kSubject, position));
    REQUIRE(!decoder.Probe().knownCandidate);
    REQUIRE(Refresh(
        decoder,
        memory,
        context,
        kProcessId + 1,
        kModuleSize + 0x1000));
    REQUIRE(!decoder.Probe().knownCandidate);
    REQUIRE(decoder.Probe().candidateCount == kCandidateCount);

    REQUIRE(decoder.Decode(kSubject, position));
    REQUIRE(!decoder.Probe().knownCandidate);
    const std::size_t codeRangeResetsBefore = harness.resetCount;
    REQUIRE(Refresh(
        decoder,
        memory,
        context,
        kProcessId + 1,
        kModuleSize + 0x1000,
        kCodeBase,
        kCodeSize + 0x1000));
    REQUIRE(!decoder.Probe().knownCandidate);
    REQUIRE(decoder.Probe().candidateCount == kCandidateCount);
    REQUIRE(decoder.Probe().codeSize == kCodeSize + 0x1000);
    REQUIRE(harness.resetCount > codeRangeResetsBefore);

    decoder.Reset();
    const auto resetProbe = decoder.Probe();
    REQUIRE(resetProbe.configured);
    REQUIRE(!resetProbe.refreshed);
    REQUIRE(resetProbe.candidateCount == 0);
    REQUIRE(!resetProbe.knownCandidate);
    REQUIRE(resetProbe.stage ==
            game::CoordinateExecutionDecoderStage::Configured);
}

void TestSharedModeDoesNotTraverseCandidates() {
    SparseMemory memory = BuildCandidateMemory();
    Harness harness;
    game::CoordinateExecutionDecoder decoder(harness.Hooks());
    for (const auto mode : {
             game::CoordinateExecutionMode::Interpret,
             game::CoordinateExecutionMode::Predecode,
             game::CoordinateExecutionMode::Jit,
         }) {
        harness.requests.clear();
        harness.responder = [&harness](const auto&) {
            return harness.requests.size() == 1
                ? Failure(game::CoordinateExecutionStatus::
                      ReadOrCoordinateFailure)
                : Success();
        };
        REQUIRE(decoder.Configure(mode, SyntheticLayout()));
        REQUIRE(Refresh(decoder, memory, ExecutionContext(1)));

        game::CoordinateExecutionPosition position{};
        REQUIRE(!decoder.Decode(kSubject, position));
        REQUIRE(harness.requests.size() == 1);
        REQUIRE(harness.requests.front().candidate.q0 == CandidateQ0(0));
        REQUIRE(!harness.requests.front().candidateKnown);
        REQUIRE(decoder.Probe().cursor == 0);
        REQUIRE(!decoder.Probe().knownCandidate);
        REQUIRE(decoder.Probe().status ==
                game::CoordinateExecutionStatus::ReadOrCoordinateFailure);
    }
}

void TestMode1BackendFailureClassification() {
    SparseMemory memory = BuildCandidateMemory();
    Harness harness;
    game::CoordinateExecutionDecoder decoder(harness.Hooks());
    REQUIRE(decoder.Configure(
        game::CoordinateExecutionMode::Emulate, SyntheticLayout()));
    REQUIRE(Refresh(decoder, memory, ExecutionContext(1)));

    game::CoordinateExecutionPosition position{};
    REQUIRE(!decoder.Decode(kSubject, position));
    REQUIRE(decoder.Probe().cursor == 64);

    harness.requests.clear();
    harness.runtimeProbe.error =
        game::CoordinateExecutionRuntimeError::EngineSetupFailed;
    harness.responder = [](const auto&) {
        return Failure(game::CoordinateExecutionStatus::BackendUnavailable);
    };
    REQUIRE(!decoder.Decode(kSubject, position));
    REQUIRE(harness.requests.size() == 1);
    REQUIRE(decoder.Probe().cursor == 64);
    REQUIRE(decoder.Probe().status ==
            game::CoordinateExecutionStatus::BackendUnavailable);

    decoder.Reset();
    REQUIRE(decoder.Configure(
        game::CoordinateExecutionMode::Emulate, SyntheticLayout()));
    REQUIRE(Refresh(decoder, memory, ExecutionContext(1)));
    harness.requests.clear();
    harness.runtimeProbe.error = game::CoordinateExecutionRuntimeError::None;
    harness.responder = [&harness](const auto&) {
        if (harness.requests.size() == 1) {
            harness.runtimeProbe.error =
                game::CoordinateExecutionRuntimeError::RemotePageReadFailed;
            return Failure(
                game::CoordinateExecutionStatus::BackendUnavailable);
        }
        harness.runtimeProbe.error =
            game::CoordinateExecutionRuntimeError::None;
        return Success();
    };
    REQUIRE(decoder.Decode(kSubject, position));
    REQUIRE(harness.requests.size() == 2);
    REQUIRE(decoder.Probe().knownCandidate);
    REQUIRE(decoder.Probe().cachedCandidate.q0 == CandidateQ0(1));
}

void TestInvalidLifecycle() {
    SparseMemory memory = BuildCandidateMemory();
    Harness harness;
    game::CoordinateExecutionDecoder decoder(harness.Hooks());
    REQUIRE(!Refresh(decoder, memory, ExecutionContext(1)));
    REQUIRE(decoder.Probe().error ==
            game::CoordinateExecutionDecoderError::InvalidRefresh);
    REQUIRE(decoder.Probe().status ==
            game::CoordinateExecutionStatus::EnvironmentFailure);
    REQUIRE(!decoder.Configure(
        static_cast<game::CoordinateExecutionMode>(0), SyntheticLayout()));
    REQUIRE(decoder.Probe().error ==
            game::CoordinateExecutionDecoderError::InvalidMode);
}

void TestRefreshDiscoversWithoutExecutionContext() {
    SparseMemory memory = BuildCandidateMemory();
    Harness harness;
    game::CoordinateExecutionDecoder decoder(harness.Hooks());
    game::ProcessExecutionContext context{};
    context.generation = 9;

    REQUIRE(decoder.Configure(
        game::CoordinateExecutionMode::Emulate, SyntheticLayout()));
    REQUIRE(Refresh(decoder, memory, context));
    const auto probe = decoder.Probe();
    REQUIRE(probe.refreshed);
    REQUIRE(probe.candidateCount == kCandidateCount);
    REQUIRE(probe.moduleBase == kModuleBase);
    REQUIRE(probe.moduleSize == kModuleSize);
    REQUIRE(probe.codeBase == kCodeBase);
    REQUIRE(probe.codeSize == kCodeSize);
    REQUIRE(probe.contextGeneration == context.generation);
}

void TestRefreshRejectsMissingCodeRange() {
    SparseMemory memory = BuildCandidateMemory();
    Harness harness;
    game::CoordinateExecutionDecoder decoder(harness.Hooks());

    REQUIRE(decoder.Configure(
        game::CoordinateExecutionMode::Emulate, SyntheticLayout()));
    REQUIRE(!Refresh(
        decoder,
        memory,
        ExecutionContext(1),
        kProcessId,
        kModuleSize,
        kCodeBase,
        0));
    const auto probe = decoder.Probe();
    REQUIRE(!probe.refreshed);
    REQUIRE(probe.moduleBase == kModuleBase);
    REQUIRE(probe.moduleSize == kModuleSize);
    REQUIRE(probe.codeBase == kCodeBase);
    REQUIRE(probe.codeSize == 0);
    REQUIRE(probe.error ==
            game::CoordinateExecutionDecoderError::InvalidRefresh);
    REQUIRE(probe.status ==
            game::CoordinateExecutionStatus::EnvironmentFailure);
}

void TestCandidateDiscoveryFailureIsInvalidAddress() {
    SparseMemory memory;
    Harness harness;
    game::CoordinateExecutionDecoder decoder(harness.Hooks());
    REQUIRE(decoder.Configure(
        game::CoordinateExecutionMode::Emulate, SyntheticLayout()));
    REQUIRE(!Refresh(decoder, memory, ExecutionContext(1)));
    const auto probe = decoder.Probe();
    REQUIRE(probe.error == game::CoordinateExecutionDecoderError::
            CandidateDiscoveryFailed);
    REQUIRE(probe.status ==
            game::CoordinateExecutionStatus::InvalidAddress);
}

}  // namespace

int main() {
    TestUnknownLimitAndPersistentCursor();
    TestTraversalTimeLimitBetweenCandidates();
    TestKnownCandidateIsExclusive();
    TestSharedEntryMappingAndInvalidation();
    TestSharedModeDoesNotTraverseCandidates();
    TestMode1BackendFailureClassification();
    TestInvalidLifecycle();
    TestRefreshDiscoversWithoutExecutionContext();
    TestRefreshRejectsMissingCodeRange();
    TestCandidateDiscoveryFailureIsInvalidAddress();
    return 0;
}
