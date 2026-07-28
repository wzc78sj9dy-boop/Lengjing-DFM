#include "game/native/SceneTransformRuntime.h"

#include "game/native/MemoryTransport.h"
#include "game/native/SecureKernelClient.h"

#include <unicorn/unicorn.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/syscall.h>
#include <unistd.h>

#ifndef LENGJING_ENABLE_COORDINATE_DEBUG_LOG
#define LENGJING_ENABLE_COORDINATE_DEBUG_LOG 0
#endif

namespace lengjing::game::native {
namespace {

constexpr std::uint64_t kPageSize = UINT64_C(0x1000);
constexpr std::uint64_t kPageMask = ~(kPageSize - 1);
constexpr std::uint64_t kEntryReadSize = UINT64_C(0x8000);
constexpr std::uint64_t kContextSize = UINT64_C(0x310);
constexpr std::uint64_t kContextSelectedOffset = UINT64_C(0x200);
constexpr std::uint64_t kContextFrameOffset = UINT64_C(0x2E8);
constexpr std::uint64_t kContextReturnOffset = UINT64_C(0x2F0);
constexpr std::uint64_t kFrameDistance = UINT64_C(0x40);
constexpr std::uint64_t kContextDistance = UINT64_C(0x350);
constexpr std::uint64_t kSyntheticStackLow =
    UINT64_C(0x00000D0000000000);
constexpr std::uint64_t kSyntheticStackSize = UINT64_C(0x40000);
constexpr std::uint64_t kSyntheticStackHigh =
    kSyntheticStackLow + kSyntheticStackSize;
constexpr std::uint64_t kStackSelectionPadding = UINT64_C(0x2000);
constexpr std::uint64_t kStackGuardSize = UINT64_C(0x1000);
constexpr std::uint64_t kStackSelectionBias = UINT64_C(0x103F);
constexpr std::uint64_t kTlsArenaMask = UINT64_C(0x1FFFFFFFFC0);
constexpr std::uint64_t kFallbackArenaMask = UINT64_C(0x1FFFFFFF000);
constexpr std::uint64_t kArenaAlignmentMask =
    UINT64_C(0xFFFFFFFFFFFFFFC0);
constexpr std::size_t kMaximumMappedPages = 1024;
constexpr std::size_t kMaximumPendingSubjects = 256;
constexpr std::uint32_t kMaximumAttempts = 5;
constexpr std::uint32_t kStopReturned = 1;
constexpr std::uint32_t kStopSelectedWrite = 2;
constexpr std::uint32_t kStopStepLimit = 3;
constexpr std::uint32_t kStopError = 4;
constexpr std::uint32_t kPacgaMask = UINT32_C(0xFFE0FC00);
constexpr std::uint32_t kPacgaOpcode = UINT32_C(0x9AC03000);
constexpr std::uint32_t kSvcMask = UINT32_C(0xFFE0001F);
constexpr std::uint32_t kSvcOpcode = UINT32_C(0xD4000001);
constexpr std::uint32_t kBranchLinkMask = UINT32_C(0xFC000000);
constexpr std::uint32_t kBranchLinkOpcode = UINT32_C(0x94000000);
constexpr std::uint32_t kBranchLinkRegisterMask =
    UINT32_C(0xFFFFFC1F);
constexpr std::uint32_t kBranchLinkRegisterOpcode =
    UINT32_C(0xD63F0000);
constexpr std::uint32_t kReturnRegisterOpcode =
    UINT32_C(0xD65F0000);
constexpr std::uint64_t kSystemCallIoctl = UINT64_C(29);
constexpr std::uint64_t kSystemCallSeek = UINT64_C(62);
constexpr std::uint64_t kSystemCallThreadId = UINT64_C(178);
constexpr std::uint64_t kSystemCallWriteVector = UINT64_C(271);
constexpr std::uint32_t kLookupIoctl = UINT32_C(0xC0300947);
constexpr std::uint32_t kAuxiliaryIoctl = UINT32_C(0xC030094C);
constexpr std::uint32_t kLearnInstructionMask =
    UINT32_C(0xFF20001F);
constexpr std::uint32_t kLearnInstructionOpcode =
    UINT32_C(0xEB00001F);
constexpr std::uint64_t kLearnWindow = UINT64_C(0x100);
constexpr std::uint64_t kMaximumIoVectorCount = UINT64_C(0x400);
constexpr std::size_t kIoTransferChunkSize = 0x1000;
constexpr std::size_t kMaximumCallTraceEntries = 128;
constexpr float kReferenceDistanceSquared = 2500000000.0F;
constexpr std::uint32_t kThreadKeyBase = UINT32_C(0x80000000);
constexpr std::uint32_t kThreadKeyCount = UINT32_C(0x82);
constexpr std::uint64_t kThreadKeyEntrySize = UINT64_C(0x10);
constexpr std::uint64_t kThreadSpecificValueOffset = UINT64_C(8);
constexpr std::uint64_t kThreadKeyAdrOffset = UINT64_C(0x1C);
constexpr std::uint32_t kAdrMask = UINT32_C(0x9F000000);
constexpr std::uint32_t kAdrOpcode = UINT32_C(0x10000000);
constexpr std::uint64_t kAdrSignBit = UINT64_C(0x100000);
constexpr std::uint64_t kAdrImmediateMask = UINT64_C(0x1FFFFC);

constexpr std::uint64_t AlignDown(
    std::uint64_t value,
    std::uint64_t alignment) noexcept {
    return value & ~(alignment - 1);
}

constexpr bool AddAddress(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& result) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        result = 0;
        return false;
    }
    result = left + right;
    return true;
}

constexpr bool IsTaggedMemoryInstruction(
    std::uint32_t instruction) noexcept {
    return (instruction & UINT32_C(0x3B000000)) ==
            UINT32_C(0x39000000) ||
        (instruction & UINT32_C(0x3B200C00)) ==
            UINT32_C(0x38200800) ||
        (instruction & UINT32_C(0x3B200C00)) ==
            UINT32_C(0x38200000);
}

constexpr std::uint32_t MemoryBaseRegister(
    std::uint32_t instruction) noexcept {
    return (instruction >> 5U) & UINT32_C(0x1F);
}

int XRegisterId(std::uint32_t index) noexcept {
    if (index <= 28) {
        return UC_ARM64_REG_X0 + static_cast<int>(index);
    }
    if (index == 29) return UC_ARM64_REG_X29;
    if (index == 30) return UC_ARM64_REG_X30;
    return UC_ARM64_REG_XZR;
}

std::uint64_t ReadHostCtrEl0() noexcept {
#if defined(__aarch64__)
    std::uint64_t value = 0;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(value));
    return value;
#else
    return UINT64_C(0x8444C004);
#endif
}

std::uint64_t ReadHostCounterFrequency() noexcept {
#if defined(__aarch64__)
    std::uint64_t value = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
#else
    return UINT64_C(19200000);
#endif
}

std::uint64_t ReadHostVirtualCounter() noexcept {
#if defined(__aarch64__)
    std::uint64_t value = 0;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
#else
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

bool IsFinite(const StableCoordinate& value) noexcept {
    return std::isfinite(value[0]) && std::isfinite(value[1]) &&
        std::isfinite(value[2]);
}

float XyDistanceSquared(
    const StableCoordinate& left,
    const StableCoordinate& right) noexcept {
    const float dx = left[0] - right[0];
    const float dy = left[1] - right[1];
    return dx * dx + dy * dy;
}

std::uint64_t ShiftRegisterValue(
    std::uint64_t value,
    std::uint32_t type,
    std::uint32_t amount) noexcept {
    switch (type) {
        case 0:
            return value << amount;
        case 1:
            return value >> amount;
        case 2:
            return static_cast<std::uint64_t>(
                static_cast<std::int64_t>(value) >> amount);
        case 3:
            return std::rotr(value, static_cast<int>(amount));
    }
    return value;
}

enum class ExternalCall : std::uint8_t {
    MutexLock,
    MutexTryLock,
    MutexUnlock,
    ThreadGetSpecific,
    ThreadSetSpecific,
    ReadLock,
    TryReadLock,
    WriteLock,
    TryWriteLock,
    ReadWriteUnlock,
    SpinLock,
    SpinTryLock,
    SpinUnlock,
    SystemConfiguration,
};

constexpr std::array<std::pair<std::string_view, ExternalCall>, 14>
    kExternalCalls{{
        {"pthread_mutex_lock", ExternalCall::MutexLock},
        {"pthread_mutex_trylock", ExternalCall::MutexTryLock},
        {"pthread_mutex_unlock", ExternalCall::MutexUnlock},
        {"pthread_getspecific", ExternalCall::ThreadGetSpecific},
        {"pthread_setspecific", ExternalCall::ThreadSetSpecific},
        {"pthread_rwlock_rdlock", ExternalCall::ReadLock},
        {"pthread_rwlock_tryrdlock", ExternalCall::TryReadLock},
        {"pthread_rwlock_wrlock", ExternalCall::WriteLock},
        {"pthread_rwlock_trywrlock", ExternalCall::TryWriteLock},
        {"pthread_rwlock_unlock", ExternalCall::ReadWriteUnlock},
        {"pthread_spin_lock", ExternalCall::SpinLock},
        {"pthread_spin_trylock", ExternalCall::SpinTryLock},
        {"pthread_spin_unlock", ExternalCall::SpinUnlock},
        {"sysconf", ExternalCall::SystemConfiguration},
    }};

#pragma pack(push, 1)
struct RemoteTransform {
    std::array<float, 4> rotation{};
    StableCoordinate translation{};
    StableCoordinate scale{};
};

struct GuestIoVector {
    std::uint64_t base = 0;
    std::uint64_t length = 0;
};
#pragma pack(pop)

static_assert(sizeof(RemoteTransform) == 0x28);
static_assert(sizeof(GuestIoVector) == 0x10);

bool IsValidTransform(const RemoteTransform& transform) noexcept {
    for (const float value : transform.rotation) {
        if (!std::isfinite(value)) return false;
    }
    for (const float value : transform.translation) {
        if (!std::isfinite(value)) return false;
    }
    for (const float value : transform.scale) {
        if (!std::isfinite(value)) return false;
    }
    return transform.translation[0] != 0.0F ||
        transform.translation[1] != 0.0F ||
        transform.translation[2] != 0.0F;
}

void WriteCoordinateLog(const SceneTransformProbe& probe) noexcept {
#if LENGJING_ENABLE_COORDINATE_DEBUG_LOG
    char line[1400]{};
    const int length = std::snprintf(
        line,
        sizeof(line),
        "[coordinate-runtime] ready=%d meta=%d err=%s sys=%d "
        "gen=%llu module=0x%llx tid=%d tls=0x%llx "
        "stack=[0x%llx,0x%llx) root=0x%llx x0=0x%llx "
        "entry=0x%llx link=0x%llx hook=0x%llx secondary=0x%llx "
        "actor=0x%llx component=0x%llx attempt=%u reason=%u "
        "selected=0x%llx translation=(%.3f,%.3f,%.3f) "
        "base=(%.3f,%.3f,%.3f) world=(%.3f,%.3f,%.3f) "
        "decision=%s pacga=%llu insn=%llu map_hit=%llu "
        "map_learn=%llu\n",
        probe.contextReady ? 1 : 0,
        probe.metadataReady ? 1 : 0,
        SceneTransformErrorName(probe.error),
        probe.systemError,
        static_cast<unsigned long long>(probe.generation),
        static_cast<unsigned long long>(probe.moduleBase),
        static_cast<int>(probe.threadId),
        static_cast<unsigned long long>(probe.threadPointer),
        static_cast<unsigned long long>(probe.stackLow),
        static_cast<unsigned long long>(probe.stackHigh),
        static_cast<unsigned long long>(probe.invocation.root),
        static_cast<unsigned long long>(probe.invocation.initialX0),
        static_cast<unsigned long long>(probe.invocation.entryPc),
        static_cast<unsigned long long>(probe.invocation.linkPc),
        static_cast<unsigned long long>(probe.invocation.hookPc),
        static_cast<unsigned long long>(probe.invocation.secondaryEntry),
        static_cast<unsigned long long>(probe.actor),
        static_cast<unsigned long long>(probe.component),
        probe.attempt,
        probe.stopReason,
        static_cast<unsigned long long>(probe.selectedAddress),
        probe.translation[0],
        probe.translation[1],
        probe.translation[2],
        probe.actorBase[0],
        probe.actorBase[1],
        probe.actorBase[2],
        probe.candidate[0],
        probe.candidate[1],
        probe.candidate[2],
        SceneTransformDecisionName(probe.decision),
        static_cast<unsigned long long>(probe.pacgaCalls),
        static_cast<unsigned long long>(probe.instructionCount),
        static_cast<unsigned long long>(probe.mapHits),
        static_cast<unsigned long long>(probe.mapLearns));
    if (length <= 0) return;
    const int descriptor = open(
        "/sdcard/Download/lengjing_coordinate_debug.txt",
        O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
        0600);
    if (descriptor < 0) return;
    const std::size_t count = std::min<std::size_t>(
        static_cast<std::size_t>(length), sizeof(line) - 1);
    static_cast<void>(write(descriptor, line, count));
    close(descriptor);
#else
    static_cast<void>(probe);
#endif
}

}  // namespace

const char* SceneTransformDecisionName(
    SceneTransformDecision decision) noexcept {
    switch (decision) {
        case SceneTransformDecision::Pending:
            return "pending";
        case SceneTransformDecision::Accepted:
            return "accepted";
        case SceneTransformDecision::RejectedRetained:
            return "rejected_retained";
        case SceneTransformDecision::InvalidRetained:
            return "invalid_retained";
        case SceneTransformDecision::InvalidCleared:
            return "invalid_cleared";
    }
    return "pending";
}

const char* SceneTransformErrorName(SceneTransformError error) noexcept {
    switch (error) {
        case SceneTransformError::None:
            return "none";
        case SceneTransformError::InvalidConfiguration:
            return "invalid_configuration";
        case SceneTransformError::ThreadContextUnavailable:
            return "thread_context_unavailable";
        case SceneTransformError::InvocationUnavailable:
            return "invocation_unavailable";
        case SceneTransformError::EntryReadFailed:
            return "entry_read_failed";
        case SceneTransformError::MetadataUnavailable:
            return "metadata_unavailable";
        case SceneTransformError::EngineSetupFailed:
            return "engine_setup_failed";
        case SceneTransformError::RegisterSetupFailed:
            return "register_setup_failed";
        case SceneTransformError::RemoteReadFailed:
            return "remote_read_failed";
        case SceneTransformError::GuestMapFailed:
            return "guest_map_failed";
        case SceneTransformError::PacgaUnavailable:
            return "pacga_unavailable";
        case SceneTransformError::UnsupportedInstruction:
            return "unsupported_instruction";
        case SceneTransformError::EmulationFailed:
            return "emulation_failed";
        case SceneTransformError::StepLimit:
            return "step_limit";
        case SceneTransformError::FreshWriteMissing:
            return "fresh_write_missing";
        case SceneTransformError::SelectedAddressInvalid:
            return "selected_address_invalid";
        case SceneTransformError::TransformReadFailed:
            return "transform_read_failed";
        case SceneTransformError::CoordinateInvalid:
            return "coordinate_invalid";
        case SceneTransformError::ReferenceRejected:
            return "reference_rejected";
        case SceneTransformError::HistoryRejected:
            return "history_rejected";
        case SceneTransformError::StaleRequest:
            return "stale_request";
    }
    return "invalid_configuration";
}

struct SceneTransformRuntime::Impl {
    struct Task {
        SceneTransformSubject subject{};
        std::uint64_t generation = 0;
    };

    struct Page {
        std::uint64_t guest = 0;
        std::uint64_t remote = 0;
        bool code = false;
        bool stack = false;
    };

    struct ExecutionResult {
        bool fresh = false;
        std::uint32_t attempt = 0;
        std::uint32_t stopReason = 0;
        std::uint64_t selected = 0;
        std::uint64_t instructions = 0;
        std::uint64_t pacgaCalls = 0;
        std::uint64_t mapHits = 0;
        std::uint64_t mapLearns = 0;
        std::uint64_t retryDiagnostic = 0;
        int systemError = 0;
        SceneTransformError error = SceneTransformError::None;
    };

    struct PublishedEntry {
        SceneTransformResult result{};
    };

    struct Metadata {
        bool valid = false;
        std::uint32_t storedRegister = 31;
        std::uint32_t loadedRegister = 31;
        std::uint64_t addImmediate = 0;
        std::uintptr_t callPc = 0;
        std::uintptr_t postLoadPc = 0;
    };

    struct PendingMapLearn {
        bool active = false;
        bool done = false;
        std::uint64_t slot = 0;
        std::uint64_t afterCallPc = 0;
        std::uint32_t key = 0;
    };

    struct CallTraceEntry {
        std::uint64_t callPc = 0;
        std::uint64_t target = 0;
        std::uint64_t returnPc = 0;
        std::uint64_t x0 = 0;
        std::uint64_t x1 = 0;
        std::uint64_t x2 = 0;
    };

    ~Impl() {
        Stop();
    }

    bool Start(MemoryTransport& candidateMemory,
               pid_t candidateProcessId,
               std::uintptr_t candidateModuleBase,
               const SceneTransformProfile& candidateProfile) noexcept {
        if (candidateProcessId <= 0 || candidateModuleBase == 0 ||
            !candidateProfile.IsValid()) {
            std::lock_guard<std::mutex> lock(mutex);
            probe.error = SceneTransformError::InvalidConfiguration;
            return false;
        }
        Stop();
        {
            std::lock_guard<std::mutex> lock(mutex);
            memory = &candidateMemory;
            processId = candidateProcessId;
            moduleBase = candidateModuleBase;
            profile = candidateProfile;
            stopping = false;
            running = true;
            initialized = false;
            world = 0;
            ++generation;
            pending.clear();
            order.clear();
            published.clear();
            probe = {};
            probe.active = true;
            probe.generation = generation;
            probe.moduleBase = moduleBase;
        }
        try {
            worker = std::thread([this]() { WorkerMain(); });
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex);
            running = false;
            stopping = true;
            probe.active = false;
            probe.error = SceneTransformError::EngineSetupFailed;
            return false;
        }
        return true;
    }

    void Stop() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            running = false;
            pending.clear();
            order.clear();
            published.clear();
            probe.active = false;
        }
        condition.notify_all();
        if (worker.joinable()) worker.join();
        CloseEngine();
        {
            std::lock_guard<std::mutex> lock(mutex);
            memory = nullptr;
            processId = -1;
            moduleBase = 0;
            world = 0;
            initialized = false;
        }
    }

    void SetWorld(std::uintptr_t candidateWorld) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        if (!running || world == candidateWorld) return;
        world = candidateWorld;
        ++generation;
        pending.clear();
        order.clear();
        published.clear();
        probe.generation = generation;
        condition.notify_all();
    }

    void Request(const SceneTransformSubject& subject) noexcept {
        if (!IsSceneTransformRemotePointer(subject.world) ||
            !IsSceneTransformRemotePointer(subject.actor) ||
            !IsSceneTransformRemotePointer(subject.component)) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (!running || stopping || world == 0 || subject.world != world) {
            return;
        }
        const bool queued = pending.find(subject.actor) != pending.end();
        pending[subject.actor] = Task{subject, generation};
        if (!queued) {
            if (order.size() >= kMaximumPendingSubjects) {
                const std::uintptr_t discarded = order.front();
                order.pop_front();
                pending.erase(discarded);
            }
            order.push_back(subject.actor);
        }
        ++probe.requested;
        condition.notify_one();
    }

    bool Lookup(const SceneTransformSubject& subject,
                SceneTransformResult& result) const noexcept {
        result = {};
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = published.find(subject.actor);
        if (found == published.end()) return false;
        const SceneTransformResult& candidate = found->second.result;
        if (candidate.world != subject.world ||
            candidate.actor != subject.actor ||
            candidate.component != subject.component ||
            candidate.generation != generation) {
            return false;
        }
        result = candidate;
        return CoordinateStabilityPolicy::IsValid(result.position);
    }

    SceneTransformProbe Probe() const noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        return probe;
    }

    void WorkerMain() noexcept {
        SceneTransformError initError = SceneTransformError::None;
        int initStatus = 0;
        const bool ready = Initialize(initError, initStatus);
        {
            std::lock_guard<std::mutex> lock(mutex);
            initialized = ready;
            probe.contextReady = ready;
            probe.metadataReady =
                metadata.valid && capture.metadataCaptured;
            probe.error = initError;
            probe.systemError = initStatus;
            probe.stackLow = stackLow;
            probe.stackHigh = stackHigh;
            probe.threadId = threadId;
            probe.threadPointer = threadPointer;
            probe.invocation = invocation;
        }
        WriteCoordinateLog(Probe());
        if (!ready) {
            CloseEngine();
            std::lock_guard<std::mutex> lock(mutex);
            running = false;
            probe.active = false;
            pending.clear();
            order.clear();
            return;
        }

        std::uint64_t stateGeneration = 0;
        std::unordered_map<std::uintptr_t, CoordinateStabilityState> states;
        while (true) {
            Task task{};
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [this]() {
                    return stopping || !order.empty();
                });
                if (stopping) break;
                const std::uintptr_t actor = order.front();
                order.pop_front();
                const auto found = pending.find(actor);
                if (found == pending.end()) continue;
                task = found->second;
                pending.erase(found);
            }
            if (task.generation != stateGeneration) {
                states.clear();
                stateGeneration = task.generation;
            }
            ProcessTask(task, states);
        }
    }

    bool Initialize(
        SceneTransformError& error,
        int& status) noexcept {
        error = SceneTransformError::None;
        status = 0;
        MemoryTransport* transport = nullptr;
        pid_t targetPid = -1;
        std::uintptr_t targetBase = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            transport = memory;
            targetPid = processId;
            targetBase = moduleBase;
        }
        if (transport == nullptr || targetPid <= 0 || targetBase == 0) {
            error = SceneTransformError::InvalidConfiguration;
            return false;
        }

        if (!transport->QueryNamedThreadTls(
                "GameThread", threadId, threadPointer, status)) {
            error = SceneTransformError::ThreadContextUnavailable;
            return false;
        }
        if (!IsSceneTransformRemotePointer(threadPointer)) {
            error = SceneTransformError::ThreadContextUnavailable;
            status = -ERANGE;
            return false;
        }
        if (!ResolveStackArena(*transport, status)) {
            error = SceneTransformError::ThreadContextUnavailable;
            return false;
        }
        if (!ResolveInvocation(invocation)) {
            error = SceneTransformError::InvocationUnavailable;
            status = lastReadStatus;
            return false;
        }
        static_cast<void>(ReadMetadata(metadata));
        const SecureExecutionCapturePlan capturePlan{
            invocation.entryPc,
            metadata.callPc,
            metadata.postLoadPc,
            metadata.storedRegister,
            metadata.loadedRegister,
            profile.initialFpsr,
            profile.initialFpcr,
            metadata.valid,
        };
        if (!transport->CaptureExecutionState(
                threadId,
                capturePlan,
                capture,
                status)) {
            error = SceneTransformError::MetadataUnavailable;
            return false;
        }
        if (!OpenEngine()) {
            error = SceneTransformError::EngineSetupFailed;
            return false;
        }
        if (!PrimeEntryPages()) {
            error = SceneTransformError::EntryReadFailed;
            return false;
        }
        ResolveExternalCalls(*transport);
        return true;
    }

    bool ReadRemote(
        std::uintptr_t address,
        void* destination,
        std::size_t size) noexcept {
        int status = 0;
        if (memory != nullptr &&
            memory->ReadSecure(address, destination, size, status)) {
            lastReadStatus = 0;
            return true;
        }
        lastReadStatus = status;
        return false;
    }

    bool SelectStackArena(
        std::uint64_t low,
        std::uint64_t high,
        std::uint64_t selectionMask) noexcept {
        if (!IsSceneTransformRemotePointer(low) ||
            !IsSceneTransformRemotePointer(high) ||
            high <= low ||
            high - low <
                kSyntheticStackSize + kStackSelectionPadding ||
            low > std::numeric_limits<std::uint64_t>::max() -
                kStackSelectionBias) {
            return false;
        }
        const std::uint64_t base =
            (low + kStackSelectionBias) & selectionMask;
        const std::uint64_t maximum =
            (high - kSyntheticStackSize - kStackGuardSize) &
            kArenaAlignmentMask;
        if (base > maximum ||
            base > std::numeric_limits<std::uint64_t>::max() -
                kSyntheticStackSize) {
            return false;
        }
        stackLow = static_cast<std::uintptr_t>(base);
        stackHigh = static_cast<std::uintptr_t>(
            base + kSyntheticStackSize);
        return true;
    }

    bool ResolveStackArenaFromTls() noexcept {
        if (threadPointer >
            std::numeric_limits<std::uintptr_t>::max() -
                UINT64_C(8)) {
            return false;
        }
        std::uint64_t nodeRaw = 0;
        if (!ReadRemote(
                threadPointer + UINT64_C(8),
                &nodeRaw,
                sizeof(nodeRaw))) {
            return false;
        }
        const std::uint64_t node =
            NormalizeSceneTransformPointer(nodeRaw);
        if (node == 0 ||
            node > std::numeric_limits<std::uint64_t>::max() -
                UINT64_C(0x38)) {
            return false;
        }

        std::uint64_t owner = 0;
        std::array<std::uint64_t, 4> layout{};
        if (!ReadRemote(
                node + UINT64_C(0x10),
                &owner,
                sizeof(owner)) ||
            !ReadRemote(
                node + UINT64_C(0x18),
                layout.data(),
                sizeof(layout))) {
            return false;
        }
        if (static_cast<std::uint32_t>(owner) !=
                static_cast<std::uint32_t>(threadId) ||
            static_cast<std::uint32_t>(owner >> 32U) !=
                static_cast<std::uint32_t>(processId)) {
            return false;
        }

        const std::uint64_t low =
            NormalizeSceneTransformPointer(layout[1]);
        const std::uint64_t span = layout[2];
        if (low == 0 || span == 0 ||
            span > std::numeric_limits<std::uint64_t>::max() - low) {
            return false;
        }
        return SelectStackArena(low, low + span, kTlsArenaMask);
    }

    bool ResolveStackArena(
        MemoryTransport& transport,
        int& status) noexcept {
        stackLow = kSyntheticStackLow;
        stackHigh = kSyntheticStackHigh;
        if (const char* shift = std::getenv("DFM_STACK_SHIFT");
            shift != nullptr) {
            stackLow += std::strtoull(shift, nullptr, 16);
            stackHigh = stackLow + kSyntheticStackSize;
            return stackHigh > stackLow;
        }
        if (ResolveStackArenaFromTls()) return true;

        std::uintptr_t low = 0;
        std::uintptr_t high = 0;
        if (!transport.QueryThreadStack(
                threadId,
                threadPointer,
                low,
                high,
                status)) {
            return false;
        }
        return SelectStackArena(low, high, kFallbackArenaMask);
    }

    bool ResolveInvocation(
        SceneTransformInvocation& output) noexcept {
        output = {};
        std::uint64_t bridge = 0;
        std::uint64_t bridgeAddress = 0;
        if (!AddAddress(moduleBase, profile.bridgeRva, bridgeAddress) ||
            !AddAddress(
                bridgeAddress,
                profile.bridgePointerOffset,
                bridgeAddress) ||
            !ReadRemote(
                bridgeAddress, &bridge, sizeof(bridge))) {
            return false;
        }
        bridge = NormalizeSceneTransformPointer(bridge);
        if (bridge == 0 || bridge < profile.contextBackOffset) return false;

        std::uint64_t initial = 0;
        std::uint64_t entry = 0;
        if (!ReadRemote(
                bridge - profile.contextBackOffset,
                &initial,
                sizeof(initial)) ||
            !ReadRemote(
                bridge + profile.entryOffset,
                &entry,
                sizeof(entry))) {
            return false;
        }
        initial = NormalizeSceneTransformPointer(initial);
        entry = NormalizeSceneTransformPointer(entry);

        std::uint64_t link = 0;
        if (!AddAddress(bridge, profile.returnOffset, link)) return false;
        link = NormalizeSceneTransformPointer(link);

        std::uint64_t hook = 0;
        if (profile.entryOffset < UINT64_C(0x0C) ||
            !AddAddress(
                bridge,
                profile.entryOffset - UINT64_C(0x0C),
                hook)) {
            return false;
        }
        hook = NormalizeSceneTransformPointer(hook);

        std::uint64_t secondaryRootAddress = 0;
        std::uint64_t secondaryRoot = 0;
        std::uint64_t secondaryPointer = 0;
        if (!AddAddress(
                moduleBase,
                profile.secondaryRootRva,
                secondaryRootAddress) ||
            !ReadRemote(
                secondaryRootAddress,
                &secondaryRoot,
                sizeof(secondaryRoot))) {
            return false;
        }
        secondaryRoot =
            NormalizeSceneTransformPointer(secondaryRoot);
        if (secondaryRoot == 0 ||
            !ReadRemote(
                secondaryRoot + profile.secondaryPointerOffset,
                &secondaryPointer,
                sizeof(secondaryPointer)) ||
            !AddAddress(
                NormalizeSceneTransformPointer(secondaryPointer),
                profile.secondaryFinalOffset,
                secondaryPointer)) {
            return false;
        }
        secondaryPointer =
            NormalizeSceneTransformPointer(secondaryPointer);

        output = {
            static_cast<std::uintptr_t>(bridge),
            static_cast<std::uintptr_t>(initial),
            static_cast<std::uintptr_t>(entry),
            static_cast<std::uintptr_t>(link),
            static_cast<std::uintptr_t>(hook),
            static_cast<std::uintptr_t>(secondaryPointer),
        };
        return output.IsValid();
    }

    bool ReadMetadata(
        Metadata& output) noexcept {
        output = {};
        std::array<std::uint32_t, kEntryReadSize / sizeof(std::uint32_t)>
            instructions{};
        for (std::uint64_t offset = 0;
             offset < kEntryReadSize;
             offset += kPageSize) {
            if (!ReadRemote(
                    invocation.entryPc + offset,
                    reinterpret_cast<std::uint8_t*>(
                        instructions.data()) + offset,
                    kPageSize)) {
                return false;
            }
        }
        for (std::size_t seedIndex = 0;
             seedIndex < instructions.size();
             ++seedIndex) {
            const std::uint32_t seed = instructions[seedIndex];
            if ((seed & UINT32_C(0x7FFFFFFF)) !=
                UINT32_C(0x528128E1)) {
                continue;
            }
            const std::size_t begin =
                seedIndex >= 8 ? seedIndex - 8 : 0;
            const std::size_t boundedSeed =
                std::min<std::size_t>(seedIndex, 0x1FEF);
            const std::size_t end = std::min<std::size_t>(
                instructions.size(), boundedSeed + 0x11);
            bool hasAdd = false;
            bool hasCompanion = false;
            std::size_t callIndex = instructions.size();
            std::uint64_t addImmediate = 0;
            for (std::size_t index = begin; index < end; ++index) {
                const std::uint32_t instruction = instructions[index];
                if ((instruction & UINT32_C(0xFF0003FF)) ==
                    UINT32_C(0x910003E2)) {
                    addImmediate =
                        (instruction >> 10U) & UINT32_C(0xFFF);
                    if ((instruction & UINT32_C(0x00400000)) != 0) {
                        addImmediate <<= 12U;
                    }
                    hasAdd = true;
                }
                if (index >= seedIndex &&
                    (instruction & UINT32_C(0x7F9FFFFF)) ==
                        UINT32_C(0x72980601) &&
                    (instruction & UINT32_C(0x00600000)) ==
                        UINT32_C(0x00200000)) {
                    hasCompanion = true;
                }
                if (index >= seedIndex &&
                    (instruction & UINT32_C(0xFFFFFC1F)) ==
                        UINT32_C(0xD63F0000)) {
                    callIndex = index;
                    break;
                }
            }
            if (!hasAdd || !hasCompanion ||
                callIndex == instructions.size() ||
                callIndex <= begin) {
                continue;
            }

            std::uint32_t stored = 31;
            for (std::size_t index = begin; index < callIndex; ++index) {
                const std::uint32_t instruction = instructions[index];
                if ((instruction & UINT32_C(0xFFC003E0)) ==
                        UINT32_C(0xB90003E0) &&
                    ((instruction >> 8U) & UINT32_C(0x3FFC)) ==
                        addImmediate + UINT64_C(0x28)) {
                    const std::uint32_t candidate =
                        instruction & UINT32_C(0x1F);
                    if (candidate <= 30) stored = candidate;
                }
            }
            if (stored > 30) continue;

            std::uint32_t loaded = 31;
            std::size_t loadIndex = instructions.size();
            const std::size_t loadEnd = std::min<std::size_t>(
                instructions.size(), callIndex + 9);
            for (std::size_t index = callIndex + 1;
                 index < loadEnd;
                 ++index) {
                const std::uint32_t instruction = instructions[index];
                if ((instruction & UINT32_C(0xFFC003E0)) ==
                        UINT32_C(0xF94003E0) &&
                    ((instruction >> 7U) & UINT32_C(0x7FF8)) ==
                        addImmediate) {
                    const std::uint32_t candidate =
                        instruction & UINT32_C(0x1F);
                    if (candidate != 31) {
                        loaded = candidate;
                        loadIndex = index;
                        break;
                    }
                }
            }
            if (loaded == 31) continue;
            output.valid = true;
            output.storedRegister = stored;
            output.loadedRegister = loaded;
            output.addImmediate = addImmediate;
            output.callPc =
                invocation.entryPc + callIndex * sizeof(std::uint32_t);
            output.postLoadPc =
                invocation.entryPc +
                (loadIndex + 1) * sizeof(std::uint32_t);
            return true;
        }
        return false;
    }

    bool OpenEngine() noexcept {
        CloseEngine();
        if (uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &engine) != UC_ERR_OK) {
            engine = nullptr;
            return false;
        }
        static_cast<void>(uc_ctl_set_cpu_model(engine, UC_CPU_ARM64_MAX));
        if (uc_hook_add(
                engine,
                &memoryFaultHook,
                UC_HOOK_MEM_READ_UNMAPPED |
                    UC_HOOK_MEM_WRITE_UNMAPPED |
                    UC_HOOK_MEM_FETCH_UNMAPPED,
                reinterpret_cast<void*>(MemoryFaultHook),
                this,
                1,
                0) != UC_ERR_OK ||
            uc_hook_add(
                engine,
                &memoryWriteHook,
                UC_HOOK_MEM_WRITE,
                reinterpret_cast<void*>(MemoryWriteHook),
                this,
                1,
                0) != UC_ERR_OK ||
            uc_hook_add(
                engine,
                &codeHook,
                UC_HOOK_CODE,
                reinterpret_cast<void*>(CodeHook),
                this,
                1,
                0) != UC_ERR_OK ||
            uc_hook_add(
                engine,
                &mrsHook,
                UC_HOOK_INSN,
                reinterpret_cast<void*>(MrsHook),
                this,
                1,
                0,
                UC_ARM64_INS_MRS) != UC_ERR_OK) {
            CloseEngine();
            return false;
        }
        ResetEngineState();
        return true;
    }

    void ResetEngineState() noexcept {
        capturedValues.clear();
        auxiliaryValues.clear();
        seekValues.clear();
        callTrace.clear();
        pendingMapLearn = {};
        if (capture.metadataCaptured &&
            capture.storedValue != 0 &&
            capture.loadedValue != 0) {
            capturedValues[capture.storedValue] =
                capture.loadedValue;
        }
    }

    void CloseEngine() noexcept {
        pages.clear();
        externalCalls.clear();
        capturedValues.clear();
        auxiliaryValues.clear();
        seekValues.clear();
        callTrace.clear();
        pendingMapLearn = {};
        if (engine != nullptr) {
            static_cast<void>(uc_close(engine));
            engine = nullptr;
        }
        memoryFaultHook = 0;
        memoryWriteHook = 0;
        codeHook = 0;
        mrsHook = 0;
    }

    bool PrimeEntryPages() noexcept {
        if (engine == nullptr) return false;
        for (std::uint64_t offset = 0;
             offset < kEntryReadSize;
             offset += kPageSize) {
            if (!EnsurePage(
                    invocation.entryPc + offset,
                    UC_MEM_FETCH_UNMAPPED,
                    true)) {
                return false;
            }
        }
        return true;
    }

    void ResolveExternalCalls(MemoryTransport& transport) noexcept {
        externalCalls.clear();
        void* libc = dlopen("libc.so", RTLD_NOW | RTLD_LOCAL);
        if (libc == nullptr) return;
        std::size_t targetSize = 0;
        int status = 0;
        const std::uintptr_t targetBase =
            transport.ModuleBaseSecure("libc.so", targetSize, status);
        for (const auto& [name, kind] : kExternalCalls) {
            void* symbol = dlsym(libc, std::string(name).c_str());
            if (symbol == nullptr || targetBase == 0) continue;
            Dl_info info{};
            if (dladdr(symbol, &info) == 0 || info.dli_fbase == nullptr) {
                continue;
            }
            const std::uintptr_t localBase =
                reinterpret_cast<std::uintptr_t>(info.dli_fbase);
            const std::uintptr_t localSymbol =
                reinterpret_cast<std::uintptr_t>(symbol);
            if (localSymbol < localBase) continue;
            const std::uintptr_t guest =
                targetBase + (localSymbol - localBase);
            const std::uintptr_t normalized =
                NormalizeSceneTransformPointer(guest);
            if (normalized != 0) externalCalls[normalized] = kind;
        }
        dlclose(libc);
    }

    Page* FindPage(std::uint64_t guestPage) noexcept {
        const auto found = std::find_if(
            pages.begin(), pages.end(), [guestPage](const Page& page) {
                return page.guest == guestPage;
            });
        return found == pages.end() ? nullptr : &*found;
    }

    bool IsStackAddress(std::uint64_t address) const noexcept {
        return address >= stackLow && address < stackHigh;
    }

    bool EnsurePage(
        std::uint64_t guestAddress,
        uc_mem_type type,
        bool codeHint = false) noexcept {
        if (engine == nullptr || memory == nullptr) return false;
        const std::uint64_t guestPage = guestAddress & kPageMask;
        Page* existing = FindPage(guestPage);
        if (existing != nullptr) {
            if (codeHint || type == UC_MEM_FETCH_UNMAPPED) {
                existing->code = true;
            }
            return true;
        }
        if (pages.size() >= kMaximumMappedPages) return false;

        const bool stack = IsStackAddress(guestPage);
        const std::uint64_t remotePage = stack
            ? guestPage
            : (NormalizeSceneTransformPointer(guestPage) & kPageMask);
        if (!stack && !IsSceneTransformRemotePointer(remotePage)) {
            return false;
        }

        std::array<std::uint8_t, kPageSize> data{};
        if (!stack &&
            !ReadRemote(remotePage, data.data(), data.size())) {
            return false;
        }
        if (uc_mem_map(
                engine, guestPage, kPageSize, UC_PROT_ALL) != UC_ERR_OK ||
            uc_mem_write(
                engine,
                guestPage,
                data.data(),
                data.size()) != UC_ERR_OK) {
            static_cast<void>(uc_mem_unmap(
                engine, guestPage, kPageSize));
            return false;
        }
        pages.push_back(Page{
            guestPage,
            remotePage,
            codeHint || type == UC_MEM_FETCH_UNMAPPED,
            stack,
        });
        return true;
    }

    bool EnsureRange(
        std::uint64_t address,
        std::size_t size) noexcept {
        if (size == 0 ||
            size > std::numeric_limits<std::uint64_t>::max() - address) {
            return false;
        }
        const std::uint64_t end = address + size;
        for (std::uint64_t page = address & kPageMask;
             page < end;
             page += kPageSize) {
            if (!EnsurePage(page, UC_MEM_WRITE_UNMAPPED)) return false;
        }
        return true;
    }

    bool RefreshDataPages() noexcept {
        if (engine == nullptr || memory == nullptr) return false;
        std::array<std::uint8_t, kPageSize> data{};
        for (const Page& page : pages) {
            if (page.code || page.stack) continue;
            if (!ReadRemote(
                    page.remote, data.data(), data.size()) ||
                uc_mem_write(
                    engine,
                    page.guest,
                    data.data(),
                    data.size()) != UC_ERR_OK) {
                return false;
            }
        }
        return true;
    }

    bool PrepareContext(
        std::uintptr_t component,
        std::uint64_t& context,
        std::uint64_t& frame) noexcept {
        const std::uint64_t top = AlignDown(stackHigh, 16);
        if (top < stackLow + kContextDistance ||
            top < kContextDistance) {
            return false;
        }
        frame = top - kFrameDistance;
        context = top - kContextDistance;
        if (context < stackLow ||
            context + kContextSize > stackHigh ||
            !EnsureRange(context, kContextSize) ||
            !EnsureRange(stackLow + UINT64_C(0x90), 12)) {
            return false;
        }

        std::array<std::uint8_t, kContextSize> zero{};
        std::uint64_t selected = 0;
        if (!AddAddress(
                component,
                profile.componentTransformOffset,
                selected)) {
            return false;
        }
        const std::uint64_t savedFrame = frame | UINT64_C(1);
        const std::uint64_t hook = invocation.hookPc;
        const std::uint64_t sentinel = UINT64_C(0xA5A5A5A5A5A5A5A5);
        const std::uint32_t sentinelTail = UINT32_C(0xA5A5A5A5);
        if (uc_mem_write(
                engine, context, zero.data(), zero.size()) != UC_ERR_OK ||
            uc_mem_write(
                engine,
                context + kContextSelectedOffset,
                &selected,
                sizeof(selected)) != UC_ERR_OK ||
            uc_mem_write(
                engine,
                context + kContextFrameOffset,
                &savedFrame,
                sizeof(savedFrame)) != UC_ERR_OK ||
            uc_mem_write(
                engine,
                context + kContextReturnOffset,
                &hook,
                sizeof(hook)) != UC_ERR_OK ||
            uc_mem_write(
                engine,
                stackLow + UINT64_C(0x90),
                &sentinel,
                sizeof(sentinel)) != UC_ERR_OK ||
            uc_mem_write(
                engine,
                stackLow + UINT64_C(0x98),
                &sentinelTail,
                sizeof(sentinelTail)) != UC_ERR_OK) {
            return false;
        }
        selectedSlot = context + kContextSelectedOffset;
        selectedAddress = 0;
        freshWrite = false;
        return true;
    }

    bool PrepareRegisters(
        std::uint64_t context,
        std::uint64_t frame) noexcept {
        const std::uint64_t zero = 0;
        for (std::uint32_t index = 0; index <= 30; ++index) {
            if (uc_reg_write(
                    engine, XRegisterId(index), &zero) != UC_ERR_OK) {
                return false;
            }
        }
        const std::uint64_t x0 = invocation.initialX0;
        const std::uint64_t x1 = context;
        const std::uint64_t x29 = frame;
        const std::uint64_t x30 = invocation.linkPc;
        const std::uint64_t sp = context;
        const std::uint64_t pc = invocation.entryPc;
        const std::uint64_t pstate = UINT64_C(0x40000000);
        const std::uint64_t fpsr = capture.fpsr;
        const std::uint64_t fpcr = capture.fpcr;
        if (uc_reg_write(engine, UC_ARM64_REG_X0, &x0) != UC_ERR_OK ||
            uc_reg_write(engine, UC_ARM64_REG_X1, &x1) != UC_ERR_OK ||
            uc_reg_write(engine, UC_ARM64_REG_X29, &x29) != UC_ERR_OK ||
            uc_reg_write(engine, UC_ARM64_REG_X30, &x30) != UC_ERR_OK ||
            uc_reg_write(engine, UC_ARM64_REG_SP, &sp) != UC_ERR_OK ||
            uc_reg_write(engine, UC_ARM64_REG_PC, &pc) != UC_ERR_OK ||
            uc_reg_write(
                engine,
                UC_ARM64_REG_TPIDR_EL0,
                &threadPointer) != UC_ERR_OK) {
            return false;
        }
        if (uc_reg_write(
                engine, UC_ARM64_REG_PSTATE, &pstate) != UC_ERR_OK &&
            uc_reg_write(
                engine, UC_ARM64_REG_NZCV, &pstate) != UC_ERR_OK) {
            return false;
        }
        if (uc_reg_write(
                engine,
                UC_ARM64_REG_FPCR,
                &fpcr) != UC_ERR_OK ||
            uc_reg_write(
                engine,
                UC_ARM64_REG_FPSR,
                &fpsr) != UC_ERR_OK) {
            return false;
        }
        const std::array<std::uint8_t, 16> zeroVector{};
        for (int index = 0; index < 32; ++index) {
            if (uc_reg_write(
                    engine,
                    UC_ARM64_REG_V0 + index,
                    zeroVector.data()) != UC_ERR_OK) {
                return false;
            }
        }
        return true;
    }

    ExecutionResult Execute(
        std::uintptr_t component) noexcept {
        ExecutionResult result{};
        for (std::uint32_t attempt = 0;
             attempt < kMaximumAttempts;
             ++attempt) {
            result.attempt = attempt;
            activeError = SceneTransformError::None;
            activeSystemError = 0;
            stopReason = 0;
            instructionCount = 0;
            activePacgaCalls = 0;
            activeMapHits = 0;
            activeMapLearns = 0;
            activeRetryDiagnostic = 0;
            seekValues.clear();
            callTrace.clear();
            pendingMapLearn = {};
            selectedAddress = 0;
            freshWrite = false;
            if (!RefreshDataPages()) {
                result.error = SceneTransformError::RemoteReadFailed;
                result.stopReason = kStopError;
                result.systemError = lastReadStatus;
                break;
            }
            std::uint64_t context = 0;
            std::uint64_t frame = 0;
            if (!PrepareContext(component, context, frame)) {
                result.error = SceneTransformError::GuestMapFailed;
                result.stopReason = kStopError;
                break;
            }
            if (!PrepareRegisters(context, frame)) {
                result.error = SceneTransformError::RegisterSetupFailed;
                result.stopReason = kStopError;
                break;
            }

            executing = true;
            const uc_err emulation = uc_emu_start(
                engine,
                invocation.entryPc,
                invocation.linkPc,
                0,
                profile.instructionBudget);
            executing = false;
            std::uint64_t finalPc = 0;
            static_cast<void>(uc_reg_read(
                engine, UC_ARM64_REG_PC, &finalPc));
            if (stopReason == 0 &&
                instructionCount >= profile.instructionBudget) {
                stopReason = kStopStepLimit;
            }
            if (stopReason == 0 &&
                (finalPc == invocation.linkPc ||
                 finalPc == invocation.hookPc)) {
                stopReason = kStopReturned;
            }
            if (stopReason == 0 && emulation != UC_ERR_OK) {
                stopReason = kStopError;
            }
            result.stopReason = stopReason;
            result.instructions += instructionCount;
            result.pacgaCalls += activePacgaCalls;
            result.mapHits += activeMapHits;
            result.mapLearns += activeMapLearns;
            result.systemError = activeSystemError;
            result.error = activeError;
            result.retryDiagnostic = activeRetryDiagnostic;
            if (freshWrite && selectedAddress != 0) {
                result.fresh = true;
                result.selected = selectedAddress;
                result.error = SceneTransformError::None;
                break;
            }
            if (stopReason != kStopReturned ||
                result.retryDiagnostic != 0) {
                if (result.error == SceneTransformError::None) {
                    result.error = stopReason == kStopStepLimit
                        ? SceneTransformError::StepLimit
                        : SceneTransformError::EmulationFailed;
                }
                break;
            }
            result.error = SceneTransformError::FreshWriteMissing;
        }
        return result;
    }

    bool Decode(
        const Task& task,
        StableCoordinate& candidate,
        SceneTransformProbe& taskProbe,
        SceneTransformError& error) noexcept {
        candidate = {};
        error = SceneTransformError::None;
        taskProbe.actor = task.subject.actor;
        taskProbe.component = task.subject.component;

        StableCoordinate actorBase{};
        if (!ReadRemote(
                task.subject.actor + profile.actorBaseOffset,
                actorBase.data(),
                sizeof(actorBase))) {
            taskProbe.systemError = lastReadStatus;
            actorBase = {};
        } else if (!IsFinite(actorBase)) {
            actorBase = {};
        }
        taskProbe.actorBase = actorBase;

        const ExecutionResult execution = Execute(task.subject.component);
        taskProbe.attempt = execution.attempt;
        taskProbe.stopReason = execution.stopReason;
        taskProbe.pacgaCalls += execution.pacgaCalls;
        taskProbe.instructionCount += execution.instructions;
        taskProbe.mapHits += execution.mapHits;
        taskProbe.mapLearns += execution.mapLearns;
        taskProbe.systemError = execution.systemError;
        if (!execution.fresh) {
            error = execution.error == SceneTransformError::None
                ? SceneTransformError::FreshWriteMissing
                : execution.error;
            return false;
        }
        const std::uintptr_t selected =
            NormalizeSceneTransformPointer(execution.selected);
        taskProbe.selectedAddress = selected;
        if (!IsSceneTransformRemotePointer(selected)) {
            error = SceneTransformError::SelectedAddressInvalid;
            return false;
        }

        RemoteTransform transform{};
        if (!ReadRemote(
                selected, &transform, sizeof(transform))) {
            taskProbe.systemError = lastReadStatus;
            error = SceneTransformError::TransformReadFailed;
            return false;
        }
        if (!IsValidTransform(transform)) {
            error = SceneTransformError::TransformReadFailed;
            return false;
        }
        taskProbe.translation = transform.translation;
        candidate = {
            transform.translation[0] + actorBase[0],
            transform.translation[1] + actorBase[1],
            transform.translation[2] + actorBase[2],
        };
        taskProbe.candidate = candidate;
        if (!CoordinateStabilityPolicy::IsValid(candidate)) {
            error = SceneTransformError::CoordinateInvalid;
            return false;
        }
        if (CoordinateStabilityPolicy::IsValid(task.subject.reference) &&
            XyDistanceSquared(candidate, task.subject.reference) >
                kReferenceDistanceSquared) {
            error = SceneTransformError::ReferenceRejected;
            return false;
        }
        return true;
    }

    void ProcessTask(
        const Task& task,
        std::unordered_map<
            std::uintptr_t,
            CoordinateStabilityState>& states) noexcept {
        SceneTransformProbe taskProbe{};
        {
            std::lock_guard<std::mutex> lock(mutex);
            taskProbe = probe;
        }
        taskProbe.error = SceneTransformError::None;
        taskProbe.systemError = 0;
        taskProbe.attempt = 0;
        taskProbe.stopReason = 0;
        taskProbe.actor = task.subject.actor;
        taskProbe.component = task.subject.component;
        taskProbe.selectedAddress = 0;
        taskProbe.translation = {};
        taskProbe.actorBase = {};
        taskProbe.candidate = {};
        taskProbe.decision = SceneTransformDecision::Pending;
        taskProbe.pacgaCalls = 0;
        taskProbe.instructionCount = 0;
        taskProbe.mapHits = 0;
        taskProbe.mapLearns = 0;

        StableCoordinate candidate{};
        SceneTransformError decodeError = SceneTransformError::None;
        const bool decoded =
            Decode(task, candidate, taskProbe, decodeError);
        CoordinateStabilityState& state = states[task.subject.actor];
        SceneTransformDecision decision =
            SceneTransformDecision::Pending;
        bool available = false;
        if (decoded) {
            const CoordinateStabilityDecision stable =
                CoordinateStabilityPolicy::Submit(
                    state,
                    task.subject.component,
                    candidate);
            switch (stable) {
                case CoordinateStabilityDecision::Accepted:
                    decision = SceneTransformDecision::Accepted;
                    available = true;
                    break;
                case CoordinateStabilityDecision::Rejected:
                    decision =
                        SceneTransformDecision::RejectedRetained;
                    available = state.initialized;
                    decodeError = SceneTransformError::HistoryRejected;
                    break;
                case CoordinateStabilityDecision::InvalidRetained:
                    decision = SceneTransformDecision::InvalidRetained;
                    available = state.initialized;
                    break;
                case CoordinateStabilityDecision::InvalidCleared:
                    decision = SceneTransformDecision::InvalidCleared;
                    available = false;
                    decodeError = SceneTransformError::HistoryRejected;
                    break;
            }
        } else {
            const CoordinateStabilityDecision stable =
                CoordinateStabilityPolicy::RecordInvalid(
                    state, task.subject.component);
            if (stable ==
                CoordinateStabilityDecision::InvalidCleared) {
                decision = SceneTransformDecision::InvalidCleared;
                available = false;
            } else {
                decision = SceneTransformDecision::InvalidRetained;
                available = state.initialized &&
                    CoordinateStabilityPolicy::IsValid(state.current);
            }
        }
        taskProbe.decision = decision;
        taskProbe.error = decodeError;

        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping || task.generation != generation ||
                task.subject.world != world) {
                probe.error = SceneTransformError::StaleRequest;
                return;
            }
            ++probe.completed;
            switch (decision) {
                case SceneTransformDecision::Accepted:
                    ++probe.accepted;
                    break;
                case SceneTransformDecision::RejectedRetained:
                    ++probe.rejected;
                    ++probe.retained;
                    break;
                case SceneTransformDecision::InvalidRetained:
                    ++probe.retained;
                    break;
                case SceneTransformDecision::InvalidCleared:
                    ++probe.cleared;
                    break;
                case SceneTransformDecision::Pending:
                    break;
            }
            if (available) {
                published[task.subject.actor] = PublishedEntry{
                    SceneTransformResult{
                        task.subject.world,
                        task.subject.actor,
                        task.subject.component,
                        state.current,
                        decision,
                        task.generation,
                        task.subject.frameSequence,
                    },
                };
            } else {
                published.erase(task.subject.actor);
            }
            probe.error = taskProbe.error;
            probe.systemError = taskProbe.systemError;
            probe.attempt = taskProbe.attempt;
            probe.stopReason = taskProbe.stopReason;
            probe.actor = taskProbe.actor;
            probe.component = taskProbe.component;
            probe.selectedAddress = taskProbe.selectedAddress;
            probe.translation = taskProbe.translation;
            probe.actorBase = taskProbe.actorBase;
            probe.candidate = taskProbe.candidate;
            probe.decision = taskProbe.decision;
            probe.pacgaCalls += taskProbe.pacgaCalls;
            probe.instructionCount += taskProbe.instructionCount;
            probe.mapHits += taskProbe.mapHits;
            probe.mapLearns += taskProbe.mapLearns;
            taskProbe = probe;
        }
        WriteCoordinateLog(taskProbe);
    }

    static bool MemoryFaultHook(
        uc_engine*,
        uc_mem_type type,
        std::uint64_t address,
        int,
        std::int64_t,
        void* userData) {
        auto* self = static_cast<Impl*>(userData);
        if (self == nullptr) return false;
        try {
            if (type == UC_MEM_FETCH_UNMAPPED) {
                const std::uint64_t normalized =
                    NormalizeSceneTransformPointer(address);
                if (normalized != 0 && normalized != address) {
                    if (!self->EnsurePage(
                            normalized, type, true) ||
                        uc_reg_write(
                            self->engine,
                            UC_ARM64_REG_PC,
                            &normalized) != UC_ERR_OK) {
                        self->FailExecution(
                            SceneTransformError::GuestMapFailed,
                            -EFAULT);
                        return false;
                    }
                    return true;
                }
            }
            if (!self->EnsurePage(
                    address,
                    type,
                    type == UC_MEM_FETCH_UNMAPPED)) {
                self->FailExecution(
                    SceneTransformError::RemoteReadFailed,
                    self->lastReadStatus);
                return false;
            }
            return true;
        } catch (...) {
            self->FailExecution(
                SceneTransformError::GuestMapFailed,
                -ENOMEM);
            return false;
        }
    }

    static void MemoryWriteHook(
        uc_engine* engineValue,
        uc_mem_type,
        std::uint64_t address,
        int size,
        std::int64_t value,
        void* userData) {
        auto* self = static_cast<Impl*>(userData);
        if (self == nullptr || engineValue == nullptr) return;
        self->TrackSelectedWrite(
            address,
            static_cast<std::size_t>(std::max(size, 0)),
            static_cast<std::uint64_t>(value));
    }

    static void CodeHook(
        uc_engine*,
        std::uint64_t address,
        std::uint32_t,
        void* userData) {
        auto* self = static_cast<Impl*>(userData);
        if (self == nullptr || !self->executing) return;
        ++self->instructionCount;
        if (self->instructionCount >=
            self->profile.instructionBudget) {
            self->stopReason = kStopStepLimit;
            static_cast<void>(uc_emu_stop(self->engine));
            return;
        }
        if (address == self->invocation.linkPc ||
            address == self->invocation.hookPc) {
            self->stopReason = kStopReturned;
            static_cast<void>(uc_emu_stop(self->engine));
            return;
        }
        const std::uint64_t normalizedAddress =
            NormalizeSceneTransformPointer(address);
        const auto external = self->externalCalls.find(
            normalizedAddress != 0 ? normalizedAddress : address);
        if (external != self->externalCalls.end()) {
            self->HandleExternal(
                normalizedAddress != 0 ? normalizedAddress : address,
                external->second);
            return;
        }
        std::uint32_t instruction = 0;
        if (uc_mem_read(
                self->engine,
                address,
                &instruction,
                sizeof(instruction)) != UC_ERR_OK) {
            self->FailExecution(
                SceneTransformError::EmulationFailed,
                -EIO);
            return;
        }
        self->TryLearnLookupMap(address, instruction);
        if (self->stopReason == kStopError) return;
        self->ObserveCallTrace(address, instruction);
        if ((instruction & kPacgaMask) == kPacgaOpcode) {
            self->HandlePacga(address, instruction);
            return;
        }
        if ((instruction & kSvcMask) == kSvcOpcode) {
            self->HandleSvc(address);
            return;
        }
        if (IsTaggedMemoryInstruction(instruction)) {
            self->NormalizeMemoryBase(instruction);
        }
    }

    static std::uint32_t MrsHook(
        uc_engine* engineValue,
        uc_arm64_reg destination,
        const uc_arm64_cp_reg* systemRegister,
        void* userData) {
        auto* self = static_cast<Impl*>(userData);
        if (self == nullptr || systemRegister == nullptr) return 0;
        if (systemRegister->op0 != 3 || systemRegister->op1 != 3) {
            return 0;
        }
        std::uint64_t value = 0;
        if (systemRegister->crn == 0 &&
            systemRegister->crm == 0 &&
            systemRegister->op2 == 1) {
            value = ReadHostCtrEl0();
        } else if (systemRegister->crn == 13 &&
                   systemRegister->crm == 0 &&
                   systemRegister->op2 == 2) {
            value = self->threadPointer;
        } else if (systemRegister->crn == 14 &&
                   systemRegister->crm == 0 &&
                   systemRegister->op2 == 0) {
            value = ReadHostCounterFrequency();
        } else if (systemRegister->crn == 14 &&
                   systemRegister->crm == 0 &&
                   (systemRegister->op2 == 2 ||
                    systemRegister->op2 == 6)) {
            value = ReadHostVirtualCounter();
        } else {
            return 0;
        }
        if (uc_reg_write(
                engineValue, destination, &value) != UC_ERR_OK) {
            self->FailExecution(
                SceneTransformError::RegisterSetupFailed,
                -EIO);
        }
        return 1;
    }

    bool ReadX(
        std::uint32_t index,
        std::uint64_t& value) noexcept {
        value = 0;
        return index == 31 ||
            uc_reg_read(
                engine, XRegisterId(index), &value) == UC_ERR_OK;
    }

    bool WriteX(
        std::uint32_t index,
        std::uint64_t value) noexcept {
        return index == 31 ||
            uc_reg_write(
                engine, XRegisterId(index), &value) == UC_ERR_OK;
    }

    bool Advance(std::uint64_t address) noexcept {
        const std::uint64_t next = address + sizeof(std::uint32_t);
        return uc_reg_write(
            engine, UC_ARM64_REG_PC, &next) == UC_ERR_OK;
    }

    bool ReadGuest(std::uint64_t address,
                   void* destination,
                   std::size_t size) noexcept {
        return destination != nullptr &&
            EnsureRange(address, size) &&
            uc_mem_read(
                engine, address, destination, size) == UC_ERR_OK;
    }

    bool WriteGuest(std::uint64_t address,
                    const void* source,
                    std::size_t size) noexcept {
        return source != nullptr &&
            EnsureRange(address, size) &&
            uc_mem_write(
                engine, address, source, size) == UC_ERR_OK;
    }

    bool CompleteExternal(std::uint64_t result) noexcept {
        std::uint64_t link = 0;
        if (!ReadX(30, link)) return false;
        const std::uint64_t normalized =
            NormalizeSceneTransformPointer(link);
        const std::uint64_t returnPc =
            normalized != 0 ? normalized : link;
        return WriteX(0, result) &&
            uc_reg_write(
                engine, UC_ARM64_REG_PC, &returnPc) == UC_ERR_OK;
    }

    bool ResolveThreadSpecificStorage(
        std::uint64_t externalAddress,
        std::uint32_t keyIndex,
        std::uint64_t& sequence,
        std::uint64_t& slot) noexcept {
        sequence = 0;
        slot = 0;

        std::uint64_t adrAddress = 0;
        if (!AddAddress(
                externalAddress,
                kThreadKeyAdrOffset,
                adrAddress)) {
            return false;
        }
        std::uint32_t instruction = 0;
        if (!ReadGuest(
                adrAddress,
                &instruction,
                sizeof(instruction)) ||
            (instruction & kAdrMask) != kAdrOpcode ||
            adrAddress < kAdrSignBit) {
            return false;
        }

        const std::uint64_t encodedImmediate =
            ((static_cast<std::uint64_t>(instruction) >> 3U) &
             kAdrImmediateMask) |
            (static_cast<std::uint64_t>(instruction) >> 29U);
        std::uint64_t keyTable = 0;
        if (!AddAddress(
                adrAddress - kAdrSignBit,
                encodedImmediate ^ kAdrSignBit,
                keyTable)) {
            return false;
        }
        const std::uint64_t keyOffset =
            static_cast<std::uint64_t>(keyIndex) *
            kThreadKeyEntrySize;
        std::uint64_t keyEntry = 0;
        if (!AddAddress(keyTable, keyOffset, keyEntry) ||
            !ReadGuest(
                keyEntry,
                &sequence,
                sizeof(sequence))) {
            return false;
        }

        std::uint64_t tlsEntry = 0;
        if (!AddAddress(
                threadPointer,
                kThreadSpecificValueOffset,
                tlsEntry)) {
            return false;
        }
        const std::uint64_t normalizedTlsEntry =
            NormalizeSceneTransformPointer(tlsEntry);
        if (normalizedTlsEntry != 0) tlsEntry = normalizedTlsEntry;

        std::uint64_t slotBase = 0;
        if (!ReadGuest(
                tlsEntry,
                &slotBase,
                sizeof(slotBase))) {
            return false;
        }
        const std::uint64_t normalizedSlotBase =
            NormalizeSceneTransformPointer(slotBase);
        if (normalizedSlotBase != 0) {
            slotBase = normalizedSlotBase;
        }
        return AddAddress(slotBase, keyOffset, slot);
    }

    bool HandleThreadSpecific(
        std::uint64_t externalAddress,
        bool setValue) noexcept {
        std::uint64_t keyValue = 0;
        if (!ReadX(0, keyValue)) return false;
        const std::uint32_t key =
            static_cast<std::uint32_t>(keyValue);
        const std::uint32_t keyIndex = key - kThreadKeyBase;
        if (keyIndex >= kThreadKeyCount) {
            return CompleteExternal(
                setValue ? static_cast<std::uint64_t>(EINVAL) : 0);
        }

        std::uint64_t sequence = 0;
        std::uint64_t slot = 0;
        if (!ResolveThreadSpecificStorage(
                externalAddress,
                keyIndex,
                sequence,
                slot)) {
            return false;
        }
        std::uint64_t valueSlot = 0;
        if (!AddAddress(
                slot,
                kThreadSpecificValueOffset,
                valueSlot)) {
            return false;
        }

        if (setValue) {
            if ((sequence & UINT64_C(1)) == 0) {
                return CompleteExternal(
                    static_cast<std::uint64_t>(EINVAL));
            }
            std::uint64_t value = 0;
            return ReadX(1, value) &&
                WriteGuest(slot, &sequence, sizeof(sequence)) &&
                WriteGuest(valueSlot, &value, sizeof(value)) &&
                CompleteExternal(0);
        }

        if ((sequence & UINT64_C(1)) == 0) {
            const std::uint64_t zero = 0;
            return WriteGuest(
                    valueSlot, &zero, sizeof(zero)) &&
                CompleteExternal(0);
        }

        std::uint64_t storedSequence = 0;
        if (!ReadGuest(
                slot,
                &storedSequence,
                sizeof(storedSequence))) {
            return false;
        }
        if (storedSequence != sequence) {
            const std::uint64_t zero = 0;
            return WriteGuest(
                    valueSlot, &zero, sizeof(zero)) &&
                CompleteExternal(0);
        }

        std::uint64_t value = 0;
        return ReadGuest(valueSlot, &value, sizeof(value)) &&
            CompleteExternal(value);
    }

    void HandleExternal(
        std::uint64_t address,
        ExternalCall call) noexcept {
        bool completed = false;
        if (call == ExternalCall::ThreadGetSpecific ||
            call == ExternalCall::ThreadSetSpecific) {
            completed = HandleThreadSpecific(
                address,
                call == ExternalCall::ThreadSetSpecific);
        } else if (call == ExternalCall::SystemConfiguration) {
            std::uint64_t name = 0;
            if (ReadX(0, name)) {
                errno = 0;
                const long value =
                    ::sysconf(static_cast<int>(name));
                completed = CompleteExternal(
                    value >= 0
                        ? static_cast<std::uint64_t>(value)
                        : 0);
            }
        } else {
            std::uint64_t lockAddress = 0;
            if (ReadX(0, lockAddress)) {
                std::uint32_t word = 0;
                if (call != ExternalCall::MutexUnlock &&
                    call != ExternalCall::ReadWriteUnlock &&
                    call != ExternalCall::SpinUnlock) {
                    word = threadId > 1
                        ? static_cast<std::uint32_t>(threadId)
                        : 1;
                    if (const char* configured =
                            std::getenv("DFM_MUTEX_LOCK_WORD");
                        configured != nullptr) {
                        word = static_cast<std::uint32_t>(
                            std::strtoul(
                                configured, nullptr, 0));
                    }
                }
                completed =
                    (lockAddress == 0 ||
                     WriteGuest(
                         lockAddress, &word, sizeof(word))) &&
                    CompleteExternal(0);
            }
        }
        if (!completed) {
            FailExecution(
                SceneTransformError::GuestMapFailed,
                -EFAULT);
        }
    }

    void TrackSelectedWrite(
        std::uint64_t address,
        std::size_t size,
        std::uint64_t value) noexcept {
        if (!executing || selectedSlot == 0 ||
            address != selectedSlot ||
            size != sizeof(std::uint64_t)) {
            return;
        }
        std::uint64_t currentPc = 0;
        if (invocation.entryPc != 0 &&
            uc_reg_read(
                engine,
                UC_ARM64_REG_PC,
                &currentPc) == UC_ERR_OK &&
            currentPc >= invocation.entryPc &&
            currentPc - invocation.entryPc < UINT64_C(0x8000)) {
            ++activeRetryDiagnostic;
        }
        if (value == 0) return;
        freshWrite = true;
        selectedAddress = value;
        stopReason = kStopSelectedWrite;
        static_cast<void>(uc_emu_stop(engine));
    }

    bool WriteGuestTracked(
        std::uint64_t address,
        const void* source,
        std::size_t size) noexcept {
        if (!WriteGuest(address, source, size)) return false;
        if (size == sizeof(std::uint64_t)) {
            std::uint64_t value = 0;
            std::memcpy(&value, source, sizeof(value));
            TrackSelectedWrite(address, size, value);
        }
        return true;
    }

    bool ApplyLookupMap(
        std::uint64_t slot,
        std::uint32_t key,
        bool& mainHit) noexcept {
        mainHit = false;
        const auto captured = capturedValues.find(key);
        if (captured != capturedValues.end() &&
            captured->second != 0) {
            if (!WriteGuestTracked(
                    slot,
                    &captured->second,
                    sizeof(captured->second))) {
                return false;
            }
            mainHit = true;
            ++activeMapHits;
        }
        const auto auxiliary = auxiliaryValues.find(key);
        if (auxiliary != auxiliaryValues.end()) {
            static_cast<void>(WriteGuest(
                slot + UINT64_C(9),
                &auxiliary->second,
                sizeof(auxiliary->second)));
        }
        return true;
    }

    std::int64_t LearnAuxiliaryMap(
        std::uint64_t argument) noexcept {
        if (argument == 0) return 0;
        std::uint64_t flags = 0;
        std::uint32_t key = 0;
        if (!ReadGuest(argument, &flags, sizeof(flags)) ||
            !ReadGuest(
                argument + UINT64_C(0x10),
                &key,
                sizeof(key))) {
            return -EFAULT;
        }
        if (key == 0 || key > UINT32_C(0xFFFF)) return -ENOENT;
        if ((flags & UINT64_C(2)) == 0) return 0;
        std::uint32_t value = 0;
        if (!ReadGuest(
                argument + UINT64_C(0x18),
                &value,
                sizeof(value))) {
            return 0;
        }
        auxiliaryValues[key] =
            static_cast<std::uint8_t>(value);
        return 0;
    }

    bool BeginLookupIoctl(
        std::uint64_t argument,
        std::uint64_t afterCallPc,
        std::int64_t& result) noexcept {
        pendingMapLearn = {};
        result = 0;
        if (argument == 0) return true;
        std::uint32_t key = 0;
        if (!ReadGuest(
                argument + UINT64_C(0x28),
                &key,
                sizeof(key))) {
            result = -EFAULT;
            return true;
        }
        bool mainHit = false;
        if (!ApplyLookupMap(argument, key, mainHit)) return false;
        pendingMapLearn = {
            true,
            mainHit,
            argument,
            afterCallPc,
            key,
        };
        return true;
    }

    void TryLearnLookupMap(
        std::uint64_t address,
        std::uint32_t instruction) noexcept {
        if (!pendingMapLearn.active ||
            pendingMapLearn.done ||
            address < pendingMapLearn.afterCallPc ||
            address - pendingMapLearn.afterCallPc > kLearnWindow ||
            (instruction & kLearnInstructionMask) !=
                kLearnInstructionOpcode) {
            return;
        }

        const std::uint32_t first =
            (instruction >> 5U) & UINT32_C(0x1F);
        const std::uint32_t second =
            (instruction >> 16U) & UINT32_C(0x1F);
        if (first == 31 || second == 31) return;

        std::uint64_t slotValue = 0;
        std::uint64_t firstValue = 0;
        std::uint64_t secondValue = 0;
        if (!ReadGuest(
                pendingMapLearn.slot,
                &slotValue,
                sizeof(slotValue)) ||
            !ReadX(first, firstValue) ||
            !ReadX(second, secondValue)) {
            return;
        }

        const std::uint32_t shiftType =
            (instruction >> 22U) & UINT32_C(3);
        const std::uint32_t shiftAmount =
            (instruction >> 10U) & UINT32_C(0x3F);
        const std::uint64_t shifted = ShiftRegisterValue(
            secondValue, shiftType, shiftAmount);

        std::uint64_t learned = 0;
        std::uint32_t destination = 31;
        if (firstValue == slotValue && shifted != 0) {
            learned = shifted;
            destination = first;
        } else if (
            shiftAmount == 0 &&
            firstValue != 0 &&
            shifted == slotValue) {
            learned = firstValue;
            destination = second;
        } else {
            return;
        }

        if (!WriteGuestTracked(
                pendingMapLearn.slot,
                &learned,
                sizeof(learned)) ||
            !WriteX(destination, learned)) {
            return;
        }
        capturedValues[pendingMapLearn.key] = learned;
        pendingMapLearn.done = true;
        ++activeMapLearns;
    }

    void LearnSeekOnReturn(
        std::uint64_t returnTarget) noexcept {
        if (returnTarget == 0 || invocation.entryPc == 0) return;
        for (auto entry = callTrace.rbegin();
             entry != callTrace.rend();
             ++entry) {
            if (entry->returnPc != returnTarget) continue;
            std::uint64_t target =
                NormalizeSceneTransformPointer(entry->target);
            if (target == 0) target = entry->target;
            if (target >= invocation.entryPc &&
                target - invocation.entryPc < kEntryReadSize) {
                continue;
            }
            if (externalCalls.find(target) != externalCalls.end()) {
                continue;
            }
            std::uint64_t key = 0;
            std::uint64_t value = 0;
            if (!ReadX(0, key) || !ReadX(10, value)) return;
            if (key <= UINT64_C(0x100000) &&
                ((value - UINT64_C(1)) >> 32U) == 0) {
                seekValues[key] = value;
            }
            return;
        }
    }

    void ObserveCallTrace(
        std::uint64_t address,
        std::uint32_t instruction) noexcept {
        std::uint64_t target = 0;
        bool call = false;
        if ((instruction & kBranchLinkMask) ==
            kBranchLinkOpcode) {
            std::int64_t immediate =
                static_cast<std::int64_t>(
                    instruction & UINT32_C(0x03FFFFFF));
            if ((immediate & INT64_C(0x02000000)) != 0) {
                immediate |= ~INT64_C(0x03FFFFFF);
            }
            target = address +
                static_cast<std::uint64_t>(immediate * INT64_C(4));
            call = true;
        } else if (
            (instruction & kBranchLinkRegisterMask) ==
            kBranchLinkRegisterOpcode) {
            const std::uint32_t source =
                (instruction >> 5U) & UINT32_C(0x1F);
            if (source != 31 && ReadX(source, target)) call = true;
        } else if (
            (instruction & kBranchLinkRegisterMask) ==
            kReturnRegisterOpcode) {
            const std::uint32_t source =
                (instruction >> 5U) & UINT32_C(0x1F);
            if (source != 31 && ReadX(source, target)) {
                LearnSeekOnReturn(target);
            }
            return;
        }
        if (!call || target == 0) return;
        CallTraceEntry entry{};
        entry.callPc = address;
        entry.target = target;
        entry.returnPc = address + sizeof(std::uint32_t);
        if (!ReadX(0, entry.x0) ||
            !ReadX(1, entry.x1) ||
            !ReadX(2, entry.x2)) {
            return;
        }
        if (callTrace.size() >= kMaximumCallTraceEntries) {
            callTrace.erase(callTrace.begin());
        }
        callTrace.push_back(entry);
    }

    std::uint64_t FindLookupReturnPc(
        std::uint64_t argument,
        std::uint64_t fallback) const noexcept {
        const CallTraceEntry* best = nullptr;
        bool bestInside = false;
        for (const CallTraceEntry& entry : callTrace) {
            if (static_cast<std::uint32_t>(entry.x1) ==
                    kLookupIoctl &&
                entry.x2 == argument &&
                entry.returnPc != 0) {
                const bool inside =
                    entry.callPc >= invocation.entryPc &&
                    entry.callPc - invocation.entryPc <
                        kEntryReadSize;
                if (best == nullptr || inside || !bestInside) {
                    best = &entry;
                    bestInside = inside;
                }
            }
        }
        return best == nullptr ? fallback : best->returnPc;
    }

    void HandlePacga(
        std::uint64_t address,
        std::uint32_t instruction) noexcept {
        const std::uint32_t destination =
            instruction & UINT32_C(0x1F);
        const std::uint32_t source =
            (instruction >> 5U) & UINT32_C(0x1F);
        const std::uint32_t modifier =
            (instruction >> 16U) & UINT32_C(0x1F);
        std::uint64_t data = 0;
        std::uint64_t modifierValue = 0;
        if (!ReadX(source, data) ||
            !ReadX(modifier, modifierValue)) {
            FailExecution(
                SceneTransformError::RegisterSetupFailed,
                -EIO);
            return;
        }
        std::uint64_t result = 0;
        int status = 0;
        ++activePacgaCalls;
        if (memory == nullptr ||
            !memory->ExecutePacga(
                data, modifierValue, result, status)) {
            FailExecution(
                SceneTransformError::PacgaUnavailable,
                status);
            return;
        }
        if (!WriteX(destination, result) || !Advance(address)) {
            FailExecution(
                SceneTransformError::RegisterSetupFailed,
                -EIO);
        }
    }

    bool ValidateIoVector(
        const GuestIoVector& vector) noexcept {
        if (vector.length == 0) return true;
        if (vector.base >
            UINT64_C(0) - vector.length) {
            return false;
        }
        return EnsureRange(
            vector.base,
            static_cast<std::size_t>(vector.length));
    }

    static bool SumIoVectors(
        const GuestIoVector* vectors,
        std::size_t count) noexcept {
        std::uint64_t total = 0;
        for (std::size_t index = 0; index < count; ++index) {
            const std::uint64_t length = vectors[index].length;
            if (length >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()) -
                    total) {
                return false;
            }
            total += length;
        }
        return true;
    }

    bool HandleWriteVectorSyscall(
        std::uint64_t& result) noexcept {
        std::array<std::uint64_t, 6> argument{};
        for (std::uint32_t index = 0; index < argument.size(); ++index) {
            if (!ReadX(index, argument[index])) {
                FailExecution(
                    SceneTransformError::RegisterSetupFailed,
                    -EIO);
                return false;
            }
        }

        if (static_cast<std::uint32_t>(argument[0]) !=
            static_cast<std::uint32_t>(processId)) {
            result = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(-ESRCH));
            return true;
        }
        if (argument[5] != 0) {
            result = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(-EINVAL));
            return true;
        }
        if (argument[2] == 0 || argument[4] == 0) {
            result = 0;
            return true;
        }
        if (argument[2] > kMaximumIoVectorCount ||
            argument[4] > kMaximumIoVectorCount) {
            result = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(-EINVAL));
            return true;
        }

        const std::size_t localCount =
            static_cast<std::size_t>(argument[2]);
        const std::size_t remoteCount =
            static_cast<std::size_t>(argument[4]);
        std::unique_ptr<GuestIoVector[]> local(
            new (std::nothrow) GuestIoVector[localCount]{});
        std::unique_ptr<GuestIoVector[]> remote(
            new (std::nothrow) GuestIoVector[remoteCount]{});
        if (local == nullptr || remote == nullptr) {
            FailExecution(
                SceneTransformError::GuestMapFailed,
                -ENOMEM);
            return false;
        }
        if (!ReadGuest(
                argument[1],
                local.get(),
                localCount * sizeof(GuestIoVector)) ||
            !ReadGuest(
                argument[3],
                remote.get(),
                remoteCount * sizeof(GuestIoVector))) {
            result = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(-EFAULT));
            return true;
        }
        if (!SumIoVectors(local.get(), localCount) ||
            !SumIoVectors(remote.get(), remoteCount)) {
            result = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(-EINVAL));
            return true;
        }
        for (std::size_t index = 0; index < localCount; ++index) {
            if (!ValidateIoVector(local[index])) {
                result = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(-EFAULT));
                return true;
            }
        }

        std::array<std::uint8_t, kIoTransferChunkSize> scratch{};
        std::size_t localIndex = 0;
        std::size_t remoteIndex = 0;
        std::uint64_t localOffset = 0;
        std::uint64_t remoteOffset = 0;
        std::uint64_t copied = 0;
        while (true) {
            while (localIndex < localCount &&
                   localOffset == local[localIndex].length) {
                ++localIndex;
                localOffset = 0;
            }
            if (localIndex == localCount) {
                result = copied;
                return true;
            }
            while (remoteIndex < remoteCount &&
                   remoteOffset == remote[remoteIndex].length) {
                ++remoteIndex;
                remoteOffset = 0;
            }
            if (remoteIndex == remoteCount) {
                result = copied;
                return true;
            }
            if (remoteOffset == 0 &&
                !ValidateIoVector(remote[remoteIndex])) {
                result = copied != 0
                    ? copied
                    : static_cast<std::uint64_t>(
                        static_cast<std::int64_t>(-EFAULT));
                return true;
            }

            const std::uint64_t localRemaining =
                local[localIndex].length - localOffset;
            const std::uint64_t remoteRemaining =
                remote[remoteIndex].length - remoteOffset;
            const std::size_t count = static_cast<std::size_t>(
                std::min<std::uint64_t>({
                    localRemaining,
                    remoteRemaining,
                    kIoTransferChunkSize,
                }));
            if (!ReadGuest(
                    local[localIndex].base + localOffset,
                    scratch.data(),
                    count) ||
                !WriteGuestTracked(
                    remote[remoteIndex].base + remoteOffset,
                    scratch.data(),
                    count)) {
                FailExecution(
                    SceneTransformError::GuestMapFailed,
                    -EFAULT);
                return false;
            }
            localOffset += count;
            remoteOffset += count;
            copied += count;
        }
    }

    void HandleSvc(std::uint64_t address) noexcept {
        std::uint64_t number = 0;
        if (!ReadX(8, number)) {
            FailExecution(
                SceneTransformError::RegisterSetupFailed,
                -EIO);
            return;
        }
        std::uint64_t result = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(-ENOSYS));
        if (number == kSystemCallThreadId) {
            result = static_cast<std::uint64_t>(threadId);
        } else if (number == kSystemCallIoctl) {
            std::uint64_t request = 0;
            std::uint64_t argument = 0;
            if (!ReadX(1, request) ||
                !ReadX(2, argument)) {
                FailExecution(
                    SceneTransformError::RegisterSetupFailed,
                    -EIO);
                return;
            }
            if (static_cast<std::uint32_t>(request) ==
                kAuxiliaryIoctl) {
                result = static_cast<std::uint64_t>(
                    LearnAuxiliaryMap(argument));
            } else if (
                static_cast<std::uint32_t>(request) ==
                kLookupIoctl) {
                std::int64_t lookupResult = 0;
                if (!BeginLookupIoctl(
                        argument,
                        FindLookupReturnPc(
                            argument,
                            address + sizeof(std::uint32_t)),
                        lookupResult)) {
                    FailExecution(
                        SceneTransformError::GuestMapFailed,
                        -EFAULT);
                    return;
                }
                result = static_cast<std::uint64_t>(lookupResult);
            } else {
                result = 0;
            }
        } else if (number == kSystemCallSeek) {
            std::uint64_t key = 0;
            std::uint64_t offset = 0;
            std::uint64_t origin = 0;
            if (!ReadX(0, key) ||
                !ReadX(1, offset) ||
                !ReadX(2, origin)) {
                FailExecution(
                    SceneTransformError::RegisterSetupFailed,
                    -EIO);
                return;
            }
            result = 0;
            if (key <= UINT64_C(0x100000) &&
                offset == 0 && origin == 2) {
                const auto found = seekValues.find(key);
                if (found != seekValues.end()) result = found->second;
            }
        } else if (number == kSystemCallWriteVector) {
            if (!HandleWriteVectorSyscall(result)) return;
        }
        if (!WriteX(0, result) || !Advance(address)) {
            FailExecution(
                SceneTransformError::RegisterSetupFailed,
                -EIO);
        }
    }

    void NormalizeMemoryBase(
        std::uint32_t instruction) noexcept {
        const std::uint32_t index =
            MemoryBaseRegister(instruction);
        if (index == 31) return;
        std::uint64_t value = 0;
        if (!ReadX(index, value)) return;
        const std::uint64_t normalized =
            NormalizeSceneTransformPointer(value);
        if (normalized != 0 && normalized != value) {
            static_cast<void>(WriteX(index, normalized));
        }
    }

    void FailExecution(
        SceneTransformError error,
        int status) noexcept {
        if (activeError == SceneTransformError::None) {
            activeError = error;
            activeSystemError = status;
        }
        stopReason = kStopError;
        if (engine != nullptr) {
            static_cast<void>(uc_emu_stop(engine));
        }
    }

    mutable std::mutex mutex;
    std::condition_variable condition;
    std::thread worker;
    MemoryTransport* memory = nullptr;
    pid_t processId = -1;
    std::uintptr_t moduleBase = 0;
    SceneTransformProfile profile{};
    bool running = false;
    bool stopping = true;
    bool initialized = false;
    std::uintptr_t world = 0;
    std::uint64_t generation = 0;
    std::unordered_map<std::uintptr_t, Task> pending;
    std::deque<std::uintptr_t> order;
    std::unordered_map<std::uintptr_t, PublishedEntry> published;
    SceneTransformProbe probe{};

    SceneTransformInvocation invocation{};
    Metadata metadata{};
    SecureExecutionCaptureResult capture{};
    std::uintptr_t stackLow = kSyntheticStackLow;
    std::uintptr_t stackHigh = kSyntheticStackHigh;
    pid_t threadId = -1;
    std::uintptr_t threadPointer = 0;
    uc_engine* engine = nullptr;
    uc_hook memoryFaultHook = 0;
    uc_hook memoryWriteHook = 0;
    uc_hook codeHook = 0;
    uc_hook mrsHook = 0;
    std::vector<Page> pages;
    std::unordered_map<std::uintptr_t, ExternalCall> externalCalls;
    std::unordered_map<std::uint32_t, std::uint64_t> capturedValues;
    std::unordered_map<std::uint32_t, std::uint8_t> auxiliaryValues;
    std::unordered_map<std::uint64_t, std::uint64_t> seekValues;
    std::vector<CallTraceEntry> callTrace;
    PendingMapLearn pendingMapLearn{};

    bool executing = false;
    bool freshWrite = false;
    std::uint64_t selectedSlot = 0;
    std::uint64_t selectedAddress = 0;
    std::uint64_t instructionCount = 0;
    std::uint64_t activePacgaCalls = 0;
    std::uint64_t activeMapHits = 0;
    std::uint64_t activeMapLearns = 0;
    std::uint64_t activeRetryDiagnostic = 0;
    std::uint32_t stopReason = 0;
    SceneTransformError activeError = SceneTransformError::None;
    int activeSystemError = 0;
    int lastReadStatus = 0;
};

SceneTransformRuntime::SceneTransformRuntime()
    : impl_(std::make_unique<Impl>()) {}

SceneTransformRuntime::~SceneTransformRuntime() = default;

bool SceneTransformRuntime::Start(
    MemoryTransport& memory,
    pid_t processId,
    std::uintptr_t moduleBase,
    const SceneTransformProfile& profile) noexcept {
    return impl_ != nullptr &&
        impl_->Start(memory, processId, moduleBase, profile);
}

void SceneTransformRuntime::SetWorld(
    std::uintptr_t world) noexcept {
    if (impl_ != nullptr) impl_->SetWorld(world);
}

void SceneTransformRuntime::Request(
    const SceneTransformSubject& subject) noexcept {
    if (impl_ != nullptr) impl_->Request(subject);
}

bool SceneTransformRuntime::Lookup(
    const SceneTransformSubject& subject,
    SceneTransformResult& result) const noexcept {
    return impl_ != nullptr && impl_->Lookup(subject, result);
}

SceneTransformProbe SceneTransformRuntime::Probe() const noexcept {
    return impl_ != nullptr ? impl_->Probe() : SceneTransformProbe{};
}

void SceneTransformRuntime::Stop() noexcept {
    if (impl_ != nullptr) impl_->Stop();
}

}  // namespace lengjing::game::native
