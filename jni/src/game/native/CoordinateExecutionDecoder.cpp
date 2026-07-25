#include "game/native/CoordinateExecutionDecoder.h"

#include "game/native/MemoryTransport.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace lengjing::game::native {
namespace {

std::uint64_t MonotonicMilliseconds() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

bool TraversalTimeExpired(std::uint64_t started,
                          std::uint64_t current) noexcept {
    return current >= started &&
        current - started >= kCoordinateExecutionTraversalLimitMs;
}

CoordinateExecutionRequest MakeRequest(
    CoordinateExecutionMode mode,
    const CoordinateExecutionCandidate& candidate,
    bool known) noexcept {
    CoordinateExecutionRequest request{};
    request.mode = mode;
    request.candidate = candidate;
    request.candidateKnown = known;
    if (mode != CoordinateExecutionMode::Emulate) {
        request.shared.hookOffset = candidate.q1;
        request.shared.x0Override = candidate.q0;
        request.shared.absoluteEntry = candidate.q2;
        request.shared.returnStub = candidate.q3;
    }
    return request;
}

}  // namespace

struct CoordinateExecutionDecoder::Impl {
    struct Binding {
        MemoryTransport* memory = nullptr;
        CoordinateExecutionReadCallback read;
        ProcessExecutionContext executionContext{};
        std::int32_t processId = 0;
        std::uintptr_t moduleBase = 0;
        std::size_t moduleSize = 0;
        bool callbackOnly = false;
        bool valid = false;
    };

    struct CachedCandidate {
        CoordinateExecutionCandidate value{};
        std::size_t index = 0;
    };

    explicit Impl(CoordinateExecutionDecoderHooks hooksValue = {})
        : hooks(std::move(hooksValue)) {
        if (!hooks.execute) {
            runtime = std::make_unique<CoordinateExecutionRuntime>();
        }
    }

    bool Configure(CoordinateExecutionMode modeValue,
                   std::uint32_t scanProfileValue) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        try {
            if (!IsCoordinateExecutionMode(modeValue)) {
                probe.stage = CoordinateExecutionDecoderStage::Failed;
                probe.error = CoordinateExecutionDecoderError::InvalidMode;
                return false;
            }

            const bool changed = !configured || mode != modeValue ||
                scanProfile != scanProfileValue;
            mode = modeValue;
            scanProfile = scanProfileValue;
            configured = true;
            if (changed) {
                ClearCandidateState();
                ResetRuntime();
            }
            SyncConfigurationProbe();
            probe.error = CoordinateExecutionDecoderError::None;
            probe.status = CoordinateExecutionStatus::Idle;
            probe.stage = candidatesReady
                ? CoordinateExecutionDecoderStage::Ready
                : CoordinateExecutionDecoderStage::Configured;
            return true;
        } catch (...) {
            probe.stage = CoordinateExecutionDecoderStage::Failed;
            probe.error = CoordinateExecutionDecoderError::InvalidMode;
            return false;
        }
    }

    bool Refresh(MemoryTransport* memoryValue,
                 CoordinateExecutionReadCallback readValue,
                 bool callbackOnly,
                 std::int32_t processIdValue,
                 std::uintptr_t moduleBaseValue,
                 std::size_t moduleSizeValue,
                 const ProcessExecutionContext& executionContextValue)
        noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        try {
            const bool identityChanged = binding.valid &&
                (binding.processId != processIdValue ||
                 binding.moduleBase != moduleBaseValue ||
                 binding.moduleSize != moduleSizeValue ||
                 binding.executionContext.generation !=
                     executionContextValue.generation ||
                 binding.callbackOnly != callbackOnly ||
                 (!callbackOnly && binding.memory != memoryValue));
            if (identityChanged) {
                ClearCandidateState();
                binding = {};
                ResetRuntime();
            }

            if (!configured || !readValue || processIdValue <= 0 ||
                moduleBaseValue == 0 || moduleSizeValue == 0 ||
                !executionContextValue.IsUsable() ||
                (callbackOnly && !hooks.execute) ||
                (!callbackOnly && memoryValue == nullptr)) {
                probe.stage = CoordinateExecutionDecoderStage::Failed;
                probe.error =
                    CoordinateExecutionDecoderError::InvalidRefresh;
                probe.status = CoordinateExecutionStatus::BackendUnavailable;
                probe.refreshed = false;
                return false;
            }

            binding.memory = memoryValue;
            binding.read = std::move(readValue);
            binding.executionContext = executionContextValue;
            binding.processId = processIdValue;
            binding.moduleBase = moduleBaseValue;
            binding.moduleSize = moduleSizeValue;
            binding.callbackOnly = callbackOnly;
            binding.valid = true;
            SyncBindingProbe();

            if (candidatesReady && !candidates.empty()) {
                probe.stage = CoordinateExecutionDecoderStage::Ready;
                probe.error = CoordinateExecutionDecoderError::None;
                probe.status = CoordinateExecutionStatus::Idle;
                probe.refreshed = true;
                return true;
            }

            probe.stage = CoordinateExecutionDecoderStage::Discovering;
            probe.error = CoordinateExecutionDecoderError::None;
            probe.status = CoordinateExecutionStatus::Loading;
            const CoordinateExecutionCandidateScanResult discovered =
                DiscoverCoordinateExecutionCandidates(
                    binding.read,
                    CoordinateExecutionModuleSnapshot{
                        binding.moduleBase,
                        nullptr,
                        binding.moduleSize,
                    },
                    binding.moduleBase,
                    scanProfile);
            if (discovered.candidates.empty()) {
                candidatesReady = false;
                probe.candidateCount = 0;
                probe.candidatesTruncated = discovered.truncated;
                probe.refreshed = false;
                probe.stage = CoordinateExecutionDecoderStage::Failed;
                probe.error = CoordinateExecutionDecoderError::
                    CandidateDiscoveryFailed;
                probe.status = CoordinateExecutionStatus::BackendUnavailable;
                return false;
            }

            candidates = discovered.candidates;
            candidatesTruncated = discovered.truncated;
            cursor = 0;
            cachedCandidate.reset();
            candidatesReady = true;
            SyncCandidateProbe();
            probe.refreshed = true;
            probe.stage = CoordinateExecutionDecoderStage::Ready;
            probe.error = CoordinateExecutionDecoderError::None;
            probe.status = CoordinateExecutionStatus::Idle;
            return true;
        } catch (...) {
            candidatesReady = false;
            probe.refreshed = false;
            probe.stage = CoordinateExecutionDecoderStage::Failed;
            probe.error =
                CoordinateExecutionDecoderError::CandidateDiscoveryFailed;
            probe.status = CoordinateExecutionStatus::BackendUnavailable;
            return false;
        }
    }

    bool Decode(std::uintptr_t subject,
                CoordinateExecutionPosition& position) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        position = {};
        try {
            probe.attemptsThisDecode = 0;
            probe.candidateLimitReached = false;
            probe.traversalTimeLimitReached = false;
            if (!configured || !binding.valid || !candidatesReady ||
                candidates.empty()) {
                probe.stage = CoordinateExecutionDecoderStage::Failed;
                probe.error = CoordinateExecutionDecoderError::NotReady;
                probe.status = CoordinateExecutionStatus::BackendUnavailable;
                return false;
            }

            probe.stage = CoordinateExecutionDecoderStage::Executing;
            probe.error = CoordinateExecutionDecoderError::None;
            probe.status = CoordinateExecutionStatus::Loading;

            if (cachedCandidate.has_value()) {
                const CoordinateExecutionResult result = ExecuteCandidate(
                    subject,
                    cachedCandidate->value,
                    cachedCandidate->index,
                    true);
                if (result.ok != 0) {
                    return CompleteSuccess(result, position);
                }
                return CompleteFailure(result);
            }

            if (cursor >= candidates.size()) cursor = 0;
            const std::size_t startIndex = cursor;
            const std::size_t available = candidates.size() - startIndex;
            const std::size_t count = std::min(
                available, kCoordinateExecutionAttemptsPerDecode);
            const std::uint64_t started = NowMilliseconds();
            CoordinateExecutionResult lastResult{};
            lastResult.status = CoordinateExecutionStatus::EvidenceFailure;
            for (std::size_t offset = 0; offset < count; ++offset) {
                const std::size_t candidateIndex = startIndex + offset;
                lastResult = ExecuteCandidate(
                    subject,
                    candidates[candidateIndex],
                    candidateIndex,
                    false);
                if (lastResult.ok != 0) {
                    cachedCandidate = CachedCandidate{
                        candidates[candidateIndex],
                        candidateIndex,
                    };
                    cursor = 0;
                    SyncCandidateProbe();
                    return CompleteSuccess(lastResult, position);
                }

                const std::size_t next = candidateIndex + 1;
                cursor = next < candidates.size() ? next : 0;
                probe.cursor = cursor;
                if (lastResult.status ==
                    CoordinateExecutionStatus::BackendUnavailable) {
                    return CompleteFailure(lastResult);
                }
                if (next < candidates.size() &&
                    TraversalTimeExpired(started, NowMilliseconds())) {
                    probe.traversalTimeLimitReached = true;
                    break;
                }
            }

            probe.candidateLimitReached =
                count == kCoordinateExecutionAttemptsPerDecode &&
                startIndex + count < candidates.size() &&
                !probe.traversalTimeLimitReached;
            return CompleteFailure(lastResult);
        } catch (...) {
            probe.stage = CoordinateExecutionDecoderStage::Failed;
            probe.error = CoordinateExecutionDecoderError::ExecutionFailed;
            probe.status = CoordinateExecutionStatus::BackendUnavailable;
            return false;
        }
    }

    CoordinateExecutionDecoderProbe Probe() const noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        return probe;
    }

    void Reset() noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        try {
            ClearCandidateState();
            binding = {};
            ResetRuntime();
            const bool wasConfigured = configured;
            const CoordinateExecutionMode configuredMode = mode;
            const std::uint32_t configuredProfile = scanProfile;
            probe = {};
            probe.configured = wasConfigured;
            probe.mode = configuredMode;
            probe.scanProfile = configuredProfile;
            probe.stage = wasConfigured
                ? CoordinateExecutionDecoderStage::Configured
                : CoordinateExecutionDecoderStage::Idle;
        } catch (...) {
            probe.stage = CoordinateExecutionDecoderStage::Failed;
            probe.error = CoordinateExecutionDecoderError::ExecutionFailed;
        }
    }

private:
    CoordinateExecutionResult ExecuteCandidate(
        std::uintptr_t subject,
        const CoordinateExecutionCandidate& candidate,
        std::size_t candidateIndex,
        bool known) {
        const CoordinateExecutionRequest request =
            MakeRequest(mode, candidate, known);
        CoordinateExecutionResult result{};
        if (hooks.execute) {
            result = hooks.execute(subject, request);
        } else if (runtime != nullptr && binding.memory != nullptr) {
            result = runtime->Execute(
                *binding.memory,
                binding.moduleBase,
                binding.moduleSize,
                subject,
                binding.executionContext,
                request);
        } else {
            result.status = CoordinateExecutionStatus::BackendUnavailable;
        }

        ++probe.attemptsThisDecode;
        ++probe.totalAttempts;
        probe.lastCandidateIndex = candidateIndex;
        probe.lastCandidate = candidate;
        probe.status = result.status;
        if (hooks.probe) {
            probe.runtime = hooks.probe();
        } else if (runtime != nullptr) {
            probe.runtime = runtime->Probe();
        } else {
            probe.runtime = {};
        }
        return result;
    }

    bool CompleteSuccess(
        const CoordinateExecutionResult& result,
        CoordinateExecutionPosition& position) noexcept {
        position = result.position;
        probe.stage = CoordinateExecutionDecoderStage::Completed;
        probe.error = CoordinateExecutionDecoderError::None;
        probe.status = CoordinateExecutionStatus::Success;
        probe.lastObject = result.object;
        SyncCandidateProbe();
        return true;
    }

    bool CompleteFailure(
        const CoordinateExecutionResult& result) noexcept {
        probe.stage = CoordinateExecutionDecoderStage::Failed;
        probe.status = result.status;
        probe.error = result.status ==
                CoordinateExecutionStatus::BackendUnavailable
            ? CoordinateExecutionDecoderError::BackendUnavailable
            : CoordinateExecutionDecoderError::ExecutionFailed;
        SyncCandidateProbe();
        return false;
    }

    std::uint64_t NowMilliseconds() const {
        return hooks.nowMilliseconds
            ? hooks.nowMilliseconds()
            : MonotonicMilliseconds();
    }

    void ResetRuntime() {
        if (hooks.reset) {
            hooks.reset();
        } else if (runtime != nullptr) {
            runtime->Reset();
        }
        probe.runtime = {};
    }

    void ClearCandidateState() noexcept {
        candidates.clear();
        cachedCandidate.reset();
        cursor = 0;
        candidatesTruncated = false;
        candidatesReady = false;
        probe.candidateCount = 0;
        probe.cursor = 0;
        probe.refreshed = false;
        probe.attemptsThisDecode = 0;
        probe.totalAttempts = 0;
        probe.lastCandidateIndex = 0;
        probe.lastObject = 0;
        probe.candidatesTruncated = false;
        probe.knownCandidate = false;
        probe.candidateLimitReached = false;
        probe.traversalTimeLimitReached = false;
        probe.cachedCandidate = {};
        probe.lastCandidate = {};
    }

    void SyncConfigurationProbe() noexcept {
        probe.configured = configured;
        probe.mode = mode;
        probe.scanProfile = scanProfile;
    }

    void SyncBindingProbe() noexcept {
        SyncConfigurationProbe();
        probe.processId = binding.processId;
        probe.moduleBase = binding.moduleBase;
        probe.moduleSize = binding.moduleSize;
        probe.contextGeneration = binding.executionContext.generation;
    }

    void SyncCandidateProbe() noexcept {
        probe.candidateCount = candidates.size();
        probe.cursor = cursor;
        probe.candidatesTruncated = candidatesTruncated;
        probe.knownCandidate = cachedCandidate.has_value();
        probe.cachedCandidate = cachedCandidate.has_value()
            ? cachedCandidate->value
            : CoordinateExecutionCandidate{};
    }

    mutable std::mutex mutex;
    CoordinateExecutionDecoderHooks hooks;
    std::unique_ptr<CoordinateExecutionRuntime> runtime;
    Binding binding{};
    std::vector<CoordinateExecutionCandidate> candidates;
    std::optional<CachedCandidate> cachedCandidate;
    CoordinateExecutionDecoderProbe probe{};
    CoordinateExecutionMode mode = CoordinateExecutionMode::Emulate;
    std::uint32_t scanProfile = 0;
    std::size_t cursor = 0;
    bool configured = false;
    bool candidatesTruncated = false;
    bool candidatesReady = false;
};

CoordinateExecutionDecoder::CoordinateExecutionDecoder()
    : impl_(std::make_unique<Impl>()) {}

CoordinateExecutionDecoder::CoordinateExecutionDecoder(
    CoordinateExecutionDecoderHooks hooks)
    : impl_(std::make_unique<Impl>(std::move(hooks))) {}

CoordinateExecutionDecoder::~CoordinateExecutionDecoder() = default;

bool CoordinateExecutionDecoder::Configure(
    CoordinateExecutionMode mode,
    std::uint32_t scanProfile) noexcept {
    return impl_ != nullptr && impl_->Configure(mode, scanProfile);
}

bool CoordinateExecutionDecoder::Refresh(
    MemoryTransport& memory,
    std::int32_t processId,
    std::uintptr_t moduleBase,
    std::size_t moduleSize,
    const ProcessExecutionContext& executionContext) noexcept {
    CoordinateExecutionReadCallback read =
        [&memory](std::uint64_t address, void* output, std::size_t size) {
            game::CoordinateReadDiagnostic diagnostic{};
            return memory.ReadCoordinateMemory(
                static_cast<std::uintptr_t>(address),
                output,
                size,
                diagnostic);
        };
    return impl_ != nullptr && impl_->Refresh(
        &memory,
        std::move(read),
        false,
        processId,
        moduleBase,
        moduleSize,
        executionContext);
}

bool CoordinateExecutionDecoder::Refresh(
    const CoordinateExecutionReadCallback& read,
    std::int32_t processId,
    std::uintptr_t moduleBase,
    std::size_t moduleSize,
    const ProcessExecutionContext& executionContext) noexcept {
    return impl_ != nullptr && impl_->Refresh(
        nullptr,
        read,
        true,
        processId,
        moduleBase,
        moduleSize,
        executionContext);
}

bool CoordinateExecutionDecoder::Decode(
    std::uintptr_t subject,
    CoordinateExecutionPosition& position) noexcept {
    return impl_ != nullptr && impl_->Decode(subject, position);
}

CoordinateExecutionDecoderProbe CoordinateExecutionDecoder::Probe()
    const noexcept {
    return impl_ != nullptr ? impl_->Probe() : CoordinateExecutionDecoderProbe{};
}

void CoordinateExecutionDecoder::Reset() noexcept {
    if (impl_ != nullptr) impl_->Reset();
}

}  // namespace lengjing::game::native
