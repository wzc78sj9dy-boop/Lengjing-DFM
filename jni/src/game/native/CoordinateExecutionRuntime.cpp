#include "game/native/CoordinateExecutionRuntime.h"

#include "game/native/Arm64Pacga.h"
#include "game/native/MemoryTransport.h"
#include "game/native/PtraceExecutionContextProvider.h"

#include <unicorn/unicorn.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lengjing::game::native {
namespace {

constexpr std::uint64_t kPageSize = 4096;
constexpr std::uint64_t kPageMask = ~(kPageSize - 1);
constexpr std::uint64_t kSyntheticStackSize =
    kCoordinateExecutionSyntheticStackTop -
    kCoordinateExecutionSyntheticStackBase;
constexpr std::size_t kMaximumMappedPages = 4096;
constexpr std::uint32_t kCoordinateExecutionTcgBufferSize =
    UINT32_C(32) * 1024U * 1024U;
constexpr std::uint32_t kFirstFakeDescriptor = UINT32_C(0x3F000000);
constexpr std::uint64_t kAbsoluteEntryWindowSize = UINT64_C(0x4000);

constexpr std::uint32_t kPacgaMask = 0xFFE0FC00U;
constexpr std::uint32_t kPacgaOpcode = 0x9AC03000U;
constexpr std::uint32_t kSvcMask = 0xFFE0001FU;
constexpr std::uint32_t kSvcOpcode = 0xD4000001U;

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
    return 0;
#endif
}

std::uint64_t ReadHostCounterFrequency() noexcept {
#if defined(__aarch64__)
    std::uint64_t value = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
#else
    return 0;
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

bool IsFiniteCoordinate(const CoordinateExecutionPosition& value) noexcept {
    const auto finite = [](float component) {
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(component);
        return (bits & 0x7FFFFFFFU) < 0x7F800000U;
    };
    return finite(value.x) && finite(value.y) && finite(value.z);
}

CoordinateExecutionResult Failure(CoordinateExecutionStatus status) noexcept {
    CoordinateExecutionResult result{};
    result.status = status;
    return result;
}

CoordinateExecutionResult Success(
    const CoordinateExecutionPosition& position,
    std::uint64_t object) noexcept {
    CoordinateExecutionResult result{};
    result.ok = 1;
    result.status = CoordinateExecutionStatus::Success;
    result.position = position;
    result.object = NormalizeCoordinateExecutionPointer(object);
    return result;
}

}  // namespace

struct CoordinateExecutionRuntime::Impl {
    struct alignas(kPageSize) CachedPageBacking {
        std::array<std::uint8_t, kPageSize> bytes{};
    };

    struct CachedPage {
        std::uint64_t guestAddress = 0;
        std::uint64_t remoteAddress = 0;
        std::shared_ptr<CachedPageBacking> backing;
        std::vector<uc_hook> instructionHooks;
        std::vector<std::uint64_t> instructionHookAddresses;
    };

    struct FakeFile {
        std::uint64_t end = 0;
        std::uint64_t current = 0;
    };

    struct CachedPacgaOracle {
        std::uint64_t data = 0;
        std::uint64_t modifier = 0;
        std::uint64_t result = 0;
    };

    ~Impl() {
        CloseEngine();
    }

    CoordinateExecutionResult Execute(
        MemoryTransport& memoryValue,
        std::uintptr_t moduleBaseValue,
        std::size_t moduleSizeValue,
        std::uintptr_t codeBaseValue,
        std::size_t codeSizeValue,
        std::uintptr_t subjectValue,
        const ProcessExecutionContext& executionContextValue,
        const CoordinateExecutionRequest& requestValue) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        probe = {};
        request = {};
        plan = {};
        evidence = {};
        executionContext = {};
        probe.stage = CoordinateExecutionRuntimeStage::Preparing;
        probe.status = CoordinateExecutionStatus::Loading;
        probe.mode = requestValue.mode;

        try {
            if (!IsCoordinateExecutionMode(requestValue.mode)) {
                return Fail(
                    CoordinateExecutionStatus::InvalidAddress,
                    CoordinateExecutionRuntimeError::InvalidRequest);
            }
            if (!IsCoordinateExecutionPointer(subjectValue)) {
                return Fail(
                    CoordinateExecutionStatus::ReadOrCoordinateFailure,
                    CoordinateExecutionRuntimeError::InvalidRequest);
            }
            if (moduleBaseValue == 0 || moduleSizeValue == 0 ||
                codeBaseValue == 0 || codeSizeValue == 0) {
                return Fail(
                    CoordinateExecutionStatus::BackendUnavailable,
                    CoordinateExecutionRuntimeError::InvalidRequest);
            }

            const CoordinateExecutionPlan executionPlan =
                BuildCoordinateExecutionPlan(
                    moduleBaseValue,
                    moduleSizeValue,
                    codeBaseValue,
                    codeSizeValue,
                    subjectValue,
                    requestValue);
            probe.plan = executionPlan;
            if (!executionPlan.valid) {
                return Fail(
                    requestValue.mode == CoordinateExecutionMode::Emulate
                        ? CoordinateExecutionStatus::EvidenceFailure
                        : CoordinateExecutionStatus::InvalidAddress,
                    CoordinateExecutionRuntimeError::InvalidRequest);
            }

            const bool reuseEngine = CanReuseEngine(
                memoryValue,
                moduleBaseValue,
                moduleSizeValue,
                codeBaseValue,
                codeSizeValue,
                requestValue.mode,
                executionPlan,
                requestValue.layout);
            if (!reuseEngine) CloseEngine();

            memory = &memoryValue;
            codeBase = codeBaseValue;
            codeSize = codeSizeValue;
            subject = NormalizeCoordinateExecutionPointer(subjectValue);
            evidence.inputSubject = subject;
            executionContext = executionContextValue;
            exclusiveMonitorInvalid = true;
            request = requestValue;
            plan = executionPlan;
            RefreshLibcRange();
            ResetExternalCallState();
            SyncPacgaOracleCache();
            if (!ctrEl0Initialized) {
                ctrEl0 = ReadHostCtrEl0();
                ctrEl0Initialized = true;
            }
            counterFrequency = ReadHostCounterFrequency();

            if (plan.verifyReturnStubMagic && !VerifyHostReturnStubMagic()) {
                return Fail(
                    CoordinateExecutionStatus::EvidenceFailure,
                    CoordinateExecutionRuntimeError::ReturnStubInvalid);
            }
            if (engine != nullptr && !ResetEngineForRun()) {
                CloseEngine();
                memory = &memoryValue;
            }
            if (engine == nullptr &&
                (!OpenEngine() ||
                 !CaptureEngineIdentity(
                     memoryValue,
                     moduleBaseValue,
                     moduleSizeValue,
                     codeBaseValue,
                     codeSizeValue,
                     requestValue.mode,
                     executionPlan,
                     requestValue.layout))) {
                return Fail(
                    CoordinateExecutionStatus::BackendUnavailable,
                    probe.error == CoordinateExecutionRuntimeError::None
                        ? CoordinateExecutionRuntimeError::EngineSetupFailed
                        : probe.error);
            }
            if (!PrepareRegisters()) {
                return Fail(
                    CoordinateExecutionStatus::BackendUnavailable,
                    CoordinateExecutionRuntimeError::RegisterSetupFailed);
            }
            if (plan.seedSlotBeforeRun &&
                !SeedResultSlot(plan.expectedStackBase)) {
                return Fail(
                    CoordinateExecutionStatus::BackendUnavailable,
                    CoordinateExecutionRuntimeError::GuestPageWriteFailed);
            }
            if (!EnsureGuestRange(plan.entryPc, sizeof(std::uint32_t)) ||
                !EnsureGuestRange(plan.hookPc, sizeof(std::uint32_t))) {
                return Fail(
                    CoordinateExecutionStatus::BackendUnavailable,
                    probe.error == CoordinateExecutionRuntimeError::None
                        ? CoordinateExecutionRuntimeError::RemotePageReadFailed
                        : probe.error);
            }
            if (plan.verifyReturnStubMagic &&
                !VerifyGuestReturnStubMagic()) {
                return Fail(
                    CoordinateExecutionStatus::EvidenceFailure,
                    CoordinateExecutionRuntimeError::ReturnStubInvalid);
            }

            probe.stage = CoordinateExecutionRuntimeStage::Executing;
            hookFailed = false;
            hookRuntimeError = CoordinateExecutionRuntimeError::None;
            const uc_err error = uc_emu_start(
                engine,
                plan.entryPc,
                kCoordinateExecutionStopPc,
                plan.timeoutMicros,
                plan.instructionBudget);
            probe.unicornError = static_cast<int>(error);
            std::uint64_t finalPc = 0;
            const uc_err pcError = uc_reg_read(
                engine, UC_ARM64_REG_PC, &finalPc);
            evidence.finalPc = finalPc;
            evidence.hitStopPc = pcError == UC_ERR_OK &&
                finalPc == kCoordinateExecutionStopPc;
            probe.evidence = evidence;
            if (error != UC_ERR_OK || pcError != UC_ERR_OK || hookFailed) {
                return Fail(
                    CoordinateExecutionStatus::EvidenceFailure,
                    hookFailed && hookRuntimeError !=
                            CoordinateExecutionRuntimeError::None
                        ? hookRuntimeError
                        : CoordinateExecutionRuntimeError::EmulationFailed);
            }
            if (!evidence.hitStopPc) {
                return Fail(
                    CoordinateExecutionStatus::EvidenceFailure,
                    CoordinateExecutionRuntimeError::ReturnPcMismatch);
            }

            probe.stage = CoordinateExecutionRuntimeStage::Validating;
            CoordinateExecutionResult result = ValidateResult();
            probe.evidence = evidence;
            probe.status = result.status;
            if (result.ok != 0) {
                probe.stage = CoordinateExecutionRuntimeStage::Completed;
                probe.error = CoordinateExecutionRuntimeError::None;
            } else {
                probe.stage = CoordinateExecutionRuntimeStage::Failed;
                if (probe.error == CoordinateExecutionRuntimeError::None) {
                    probe.error = CoordinateExecutionRuntimeError::EvidenceInvalid;
                }
            }
            return result;
        } catch (...) {
            CloseEngine();
            return Fail(
                CoordinateExecutionStatus::BackendUnavailable,
                CoordinateExecutionRuntimeError::EngineSetupFailed);
        }
    }

    CoordinateExecutionRuntimeProbe Probe() const noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        return probe;
    }

    void Reset() noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        CloseEngine();
        ctrEl0 = 0;
        ctrEl0Initialized = false;
        probe = {};
    }

private:
    static bool MemoryFaultHook(uc_engine*,
                                uc_mem_type type,
                                std::uint64_t address,
                                int size,
                                std::int64_t value,
                                void* userData) {
        auto* self = static_cast<Impl*>(userData);
        if (self == nullptr || self->hookFailed) return false;
        self->probe.faultAddress = address;
        self->probe.faultType = static_cast<int>(type);
        self->probe.faultSize = size;
        self->probe.faultValue = value;
        try {
            static_cast<void>(self->CanonicalizeTaggedFaultBase(type));
            const bool loaded = self->MapRemotePage(address);
            if (!loaded) {
                self->hookFailed = true;
                if (self->hookRuntimeError ==
                    CoordinateExecutionRuntimeError::None) {
                    self->hookRuntimeError =
                        CoordinateExecutionRuntimeError::RemotePageReadFailed;
                }
            }
            return loaded;
        } catch (...) {
            self->enginePoisoned = true;
            self->FailHook(
                address,
                CoordinateExecutionRuntimeError::RemotePageReadFailed);
            return false;
        }
    }

    static void MemoryWriteHook(uc_engine*,
                                uc_mem_type,
                                std::uint64_t address,
                                int size,
                                std::int64_t value,
                                void* userData) {
        auto* self = static_cast<Impl*>(userData);
        if (self == nullptr || self->hookFailed) return;
        try {
            self->MarkSyntheticStackDirty(address, size);
            self->CaptureWrite(address, size, value);
        } catch (...) {
            self->enginePoisoned = true;
            self->FailHook(
                address, CoordinateExecutionRuntimeError::EmulationFailed);
        }
    }

    static void CodeHook(uc_engine*,
                         std::uint64_t address,
                         std::uint32_t,
                         void* userData) {
        auto* self = static_cast<Impl*>(userData);
        if (self == nullptr || self->hookFailed) return;
        try {
            self->HandleInstruction(address);
        } catch (...) {
            self->enginePoisoned = true;
            self->FailHook(
                address, CoordinateExecutionRuntimeError::EmulationFailed);
        }
    }

    static std::uint32_t MrsHook(uc_engine* targetEngine,
                                 uc_arm64_reg destination,
                                 const uc_arm64_cp_reg* systemRegister,
                                 void* userData) {
        auto* self = static_cast<Impl*>(userData);
        if (self == nullptr || systemRegister == nullptr ||
            systemRegister->op0 != 3 || systemRegister->op1 != 3) {
            return 0;
        }

        std::uint64_t value = 0;
        if (systemRegister->crn == 0 && systemRegister->crm == 0 &&
            systemRegister->op2 == 1) {
            value = self->ctrEl0;
        } else if (systemRegister->crn == 13 &&
                   systemRegister->crm == 0 &&
                   systemRegister->op2 == 2) {
            value = self->executionContext.tpidrEl0;
        } else if (systemRegister->crn == 14 &&
                   systemRegister->crm == 0 &&
                   systemRegister->op2 == 0) {
            value = self->counterFrequency;
        } else if (systemRegister->crn == 14 &&
                   systemRegister->crm == 0 &&
                   (systemRegister->op2 == 2 ||
                    systemRegister->op2 == 6)) {
            value = ReadHostVirtualCounter();
        } else {
            return 0;
        }
        if (uc_reg_write(targetEngine, destination, &value) != UC_ERR_OK) {
            self->FailHook(
                0, CoordinateExecutionRuntimeError::RegisterSetupFailed);
        }
        return 1;
    }

    CoordinateExecutionResult Fail(
        CoordinateExecutionStatus status,
        CoordinateExecutionRuntimeError error) noexcept {
        probe.stage = CoordinateExecutionRuntimeStage::Failed;
        probe.status = status;
        probe.error = error;
        probe.evidence = evidence;
        if (enginePoisoned) CloseEngine();
        return Failure(status);
    }

    bool CanReuseEngine(
        MemoryTransport& memoryValue,
        std::uintptr_t moduleBaseValue,
        std::size_t moduleSizeValue,
        std::uintptr_t codeBaseValue,
        std::size_t codeSizeValue,
        CoordinateExecutionMode modeValue,
        const CoordinateExecutionPlan& executionPlan,
        const CoordinateExecutionLayout& layoutValue) const noexcept {
        return engine != nullptr && !enginePoisoned && engineIdentityValid &&
            engineMemory == &memoryValue &&
            engineModuleBase == moduleBaseValue &&
            engineModuleSize == moduleSizeValue &&
            engineCodeBase == codeBaseValue &&
            engineCodeSize == codeSizeValue &&
            engineMode == modeValue && engineLayout == layoutValue &&
            SameEnginePlan(enginePlan, executionPlan);
    }

    static bool SameEnginePlan(
        const CoordinateExecutionPlan& left,
        const CoordinateExecutionPlan& right) noexcept {
        return left.entryPc == right.entryPc &&
            left.hookPc == right.hookPc &&
            left.returnStub == right.returnStub &&
            left.lr == right.lr &&
            left.expectedStackBase == right.expectedStackBase &&
            left.seedSlotBeforeRun == right.seedSlotBeforeRun &&
            left.seedSlotAtHook == right.seedSlotAtHook &&
            left.requireReturnStub == right.requireReturnStub &&
            left.verifyReturnStubMagic == right.verifyReturnStubMagic;
    }

    void RefreshLibcRange() {
        if (libcThreadId != executionContext.threadId ||
            libcThreadStartTimeTicks !=
                executionContext.threadStartTimeTicks) {
            libcThreadId = executionContext.threadId;
            libcThreadStartTimeTicks =
                executionContext.threadStartTimeTicks;
            libcBegin = 0;
            libcEnd = 0;
            MappedModuleRange range{};
            if (FindMappedModuleRange(
                    executionContext.threadId, "libc.so", range) ||
                FindMappedModuleRange(
                    executionContext.threadId, "libc-", range)) {
                libcBegin = NormalizeCoordinateExecutionPointer(range.begin);
                libcEnd = NormalizeCoordinateExecutionPointer(range.end);
                if (libcEnd <= libcBegin) {
                    libcBegin = 0;
                    libcEnd = 0;
                }
            }
        }
        evidence.libcBegin = libcBegin;
        evidence.libcEnd = libcEnd;
    }

    void ResetExternalCallState() {
        nextFakeDescriptor = kFirstFakeDescriptor;
        fakeFiles.clear();
        emptyFakeFileSeen = false;
    }

    void SyncPacgaOracleCache() {
        if (pacgaThreadId != executionContext.threadId ||
            pacgaThreadStartTimeTicks !=
                executionContext.threadStartTimeTicks ||
            pacgaTpidrEl0 != executionContext.tpidrEl0) {
            pacgaThreadId = executionContext.threadId;
            pacgaThreadStartTimeTicks =
                executionContext.threadStartTimeTicks;
            pacgaTpidrEl0 = executionContext.tpidrEl0;
            pacgaOracles.clear();
        }
        if (executionContext.pacgaOracle.available) {
            CachePacgaOracle(
                executionContext.pacgaOracle.data,
                executionContext.pacgaOracle.modifier,
                executionContext.pacgaOracle.result);
        }
    }

    bool FindPacgaOracle(std::uint64_t data,
                         std::uint64_t modifier,
                         std::uint64_t& result) const noexcept {
        for (const CachedPacgaOracle& oracle : pacgaOracles) {
            if (oracle.data == data && oracle.modifier == modifier) {
                result = oracle.result;
                return true;
            }
        }
        return false;
    }

    void CachePacgaOracle(std::uint64_t data,
                          std::uint64_t modifier,
                          std::uint64_t result) {
        for (CachedPacgaOracle& oracle : pacgaOracles) {
            if (oracle.data == data && oracle.modifier == modifier) {
                oracle.result = result;
                return;
            }
        }
        pacgaOracles.push_back({data, modifier, result});
    }

    bool CaptureEngineIdentity(
        MemoryTransport& memoryValue,
        std::uintptr_t moduleBaseValue,
        std::size_t moduleSizeValue,
        std::uintptr_t codeBaseValue,
        std::size_t codeSizeValue,
        CoordinateExecutionMode modeValue,
        const CoordinateExecutionPlan& executionPlan,
        const CoordinateExecutionLayout& layoutValue) noexcept {
        if (engine == nullptr) return false;
        engineMemory = &memoryValue;
        engineModuleBase = moduleBaseValue;
        engineModuleSize = moduleSizeValue;
        engineCodeBase = codeBaseValue;
        engineCodeSize = codeSizeValue;
        engineMode = modeValue;
        engineLayout = layoutValue;
        enginePlan = executionPlan;
        engineIdentityValid = true;
        return true;
    }

    bool RemoveInstructionHooks(CachedPage& page) noexcept {
        bool removed = true;
        if (engine != nullptr) {
            for (const uc_hook hook : page.instructionHooks) {
                removed = uc_hook_del(engine, hook) == UC_ERR_OK && removed;
            }
        }
        for (const std::uint64_t address : page.instructionHookAddresses) {
            hookedAddresses.erase(address);
        }
        page.instructionHooks.clear();
        page.instructionHookAddresses.clear();
        return removed;
    }

    bool ResetEngineForRun() {
        if (engine == nullptr || enginePoisoned) return false;

        for (auto& entry : pages) {
            CachedPage& page = entry.second;
            const std::uint64_t guestAddress = page.guestAddress;
            if (!RemoveInstructionHooks(page) ||
                uc_mem_unmap(engine, guestAddress, kPageSize) != UC_ERR_OK) {
                return false;
            }
            mappedPages.erase(guestAddress);
        }
        pages.clear();
        pageBackings.clear();
        hookedAddresses.clear();
        if (uc_ctl_flush_tb(engine) != UC_ERR_OK) return false;

        static const std::array<std::uint8_t, kPageSize> zeroPage{};
        for (const std::uint64_t page : dirtyStackPages) {
            if (uc_mem_write(
                    engine, page, zeroPage.data(), zeroPage.size()) !=
                UC_ERR_OK) {
                return false;
            }
        }
        dirtyStackPages.clear();
        hookFailed = false;
        hookRuntimeError = CoordinateExecutionRuntimeError::None;
        exclusiveMonitorInvalid = true;
        return true;
    }

    bool VerifyHostReturnStubMagic() {
        std::uint64_t magicAddress = 0;
        std::uint64_t magic = 0;
        game::CoordinateReadDiagnostic diagnostic{};
        if (!CoordinateExecutionAdd(plan.returnStub, 4, magicAddress) ||
            !ReadRemoteMemory(
                magicAddress, &magic, sizeof(magic), diagnostic)) {
            probe.read = diagnostic;
            return false;
        }
        evidence.returnStubMagicVerifiedBeforeRun =
            magic == request.layout.discovery.returnStubMagic;
        return evidence.returnStubMagicVerifiedBeforeRun;
    }

    bool VerifyGuestReturnStubMagic() {
        std::uint64_t magicAddress = 0;
        std::uint64_t magic = 0;
        if (!CoordinateExecutionAdd(plan.returnStub, 4, magicAddress) ||
            !EnsureGuestRange(magicAddress, sizeof(magic)) ||
            uc_mem_read(engine, magicAddress, &magic, sizeof(magic)) !=
                UC_ERR_OK) {
            return false;
        }
        return magic == request.layout.discovery.returnStubMagic;
    }

    bool OpenEngine() {
        if (uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &engine) != UC_ERR_OK) {
            engine = nullptr;
            probe.error = CoordinateExecutionRuntimeError::EngineSetupFailed;
            return false;
        }
        static_cast<void>(uc_ctl_set_cpu_model(engine, UC_CPU_ARM64_MAX));
        if (uc_ctl_set_tcg_buffer_size(
                engine, kCoordinateExecutionTcgBufferSize) != UC_ERR_OK ||
            uc_mem_map(
                engine,
                kCoordinateExecutionSyntheticStackBase,
                kSyntheticStackSize,
                UC_PROT_READ | UC_PROT_WRITE) != UC_ERR_OK ||
            uc_mem_map(
                engine,
                kCoordinateExecutionStopPc,
                kPageSize,
                UC_PROT_READ | UC_PROT_EXEC) != UC_ERR_OK ||
            uc_hook_add(
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
                &mrsHook,
                UC_HOOK_INSN,
                reinterpret_cast<void*>(MrsHook),
                this,
                1,
                0,
                UC_ARM64_INS_MRS) != UC_ERR_OK) {
            probe.error = CoordinateExecutionRuntimeError::EngineSetupFailed;
            CloseEngine();
            return false;
        }
        for (std::uint64_t page = kCoordinateExecutionSyntheticStackBase;
             page < kCoordinateExecutionSyntheticStackTop;
             page += kPageSize) {
            mappedPages.insert(page);
        }
        mappedPages.insert(kCoordinateExecutionStopPc);
        return true;
    }

    void CloseEngine() noexcept {
        if (engine != nullptr) {
            static_cast<void>(uc_close(engine));
            engine = nullptr;
        }
        pages.clear();
        pageBackings.clear();
        mappedPages.clear();
        hookedAddresses.clear();
        dirtyStackPages.clear();
        exclusiveMonitorInvalid = true;
        memoryFaultHook = 0;
        memoryWriteHook = 0;
        mrsHook = 0;
        memory = nullptr;
        engineMemory = nullptr;
        engineModuleBase = 0;
        engineModuleSize = 0;
        engineCodeBase = 0;
        engineCodeSize = 0;
        engineMode = static_cast<CoordinateExecutionMode>(0);
        engineLayout = {};
        enginePlan = {};
        engineIdentityValid = false;
        enginePoisoned = false;
        hookFailed = false;
        hookRuntimeError = CoordinateExecutionRuntimeError::None;
        fakeFiles.clear();
        nextFakeDescriptor = kFirstFakeDescriptor;
    }

    bool PrepareRegisters() {
        if (engine == nullptr) return false;
        const std::uint64_t zero = 0;
        for (std::uint32_t index = 0; index <= 30; ++index) {
            if (uc_reg_write(engine, XRegisterId(index), &zero) != UC_ERR_OK) {
                return false;
            }
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
        if (uc_reg_write(engine, UC_ARM64_REG_X0, &plan.x0) != UC_ERR_OK ||
            uc_reg_write(engine, UC_ARM64_REG_X1, &plan.x1) != UC_ERR_OK ||
            uc_reg_write(engine, UC_ARM64_REG_X2, &plan.x2) != UC_ERR_OK ||
            uc_reg_write(engine, UC_ARM64_REG_X29,
                         &kCoordinateExecutionDefaultFp) != UC_ERR_OK ||
            uc_reg_write(engine, UC_ARM64_REG_X30, &plan.lr) != UC_ERR_OK ||
            uc_reg_write(engine, UC_ARM64_REG_SP,
                         &kCoordinateExecutionDefaultSp) != UC_ERR_OK ||
            uc_reg_write(engine, UC_ARM64_REG_TPIDR_EL0,
                         &executionContext.tpidrEl0) != UC_ERR_OK ||
            uc_reg_write(engine, UC_ARM64_REG_NZCV, &zero) != UC_ERR_OK ||
            uc_reg_write(engine, UC_ARM64_REG_FPCR, &zero) != UC_ERR_OK ||
            uc_reg_write(engine, UC_ARM64_REG_FPSR, &zero) != UC_ERR_OK) {
            return false;
        }
        return true;
    }

    bool SeedResultSlot(std::uint64_t stackBase) {
        stackBase = NormalizeCoordinateExecutionPointer(stackBase);
        std::uint64_t slot = 0;
        if (stackBase == 0 ||
            !CoordinateExecutionAdd(
                stackBase, request.layout.result.resultSlotOffset, slot) ||
            !EnsureGuestRange(slot, sizeof(subject))) {
            return false;
        }
        if (uc_mem_write(engine, slot, &subject, sizeof(subject)) !=
            UC_ERR_OK) {
            return false;
        }
        MarkSyntheticStackDirty(slot, sizeof(subject));
        evidence.seedSubject = subject;
        evidence.seedSlot = NormalizeCoordinateExecutionPointer(slot);
        return true;
    }

    void MarkSyntheticStackDirty(std::uint64_t address,
                                 std::size_t size) {
        if (size == 0 ||
            address < kCoordinateExecutionSyntheticStackBase ||
            address >= kCoordinateExecutionSyntheticStackTop) {
            return;
        }
        const std::uint64_t maximum =
            kCoordinateExecutionSyntheticStackTop - address;
        const std::uint64_t boundedSize = std::min<std::uint64_t>(size, maximum);
        const std::uint64_t last = address + boundedSize - 1;
        for (std::uint64_t page = address & kPageMask;; page += kPageSize) {
            dirtyStackPages.insert(page);
            if (page == (last & kPageMask)) break;
        }
    }

    bool EnsureGuestRange(std::uint64_t address, std::size_t size) {
        address = NormalizeCoordinateExecutionPointer(address);
        if (engine == nullptr || size == 0 ||
            size - 1 > std::numeric_limits<std::uint64_t>::max() - address) {
            return false;
        }
        const std::uint64_t last = address + size - 1;
        std::uint64_t page = address & kPageMask;
        const std::uint64_t lastPage = last & kPageMask;
        while (true) {
            if (mappedPages.find(page) == mappedPages.end() &&
                !MapRemotePage(page)) {
                return false;
            }
            if (page == lastPage) break;
            if (page > std::numeric_limits<std::uint64_t>::max() - kPageSize) {
                return false;
            }
            page += kPageSize;
        }
        return true;
    }

    bool CanonicalizeTaggedFaultBase(uc_mem_type type) {
        if (type != UC_MEM_READ_UNMAPPED &&
            type != UC_MEM_WRITE_UNMAPPED) {
            return false;
        }
        std::uint64_t pc = 0;
        if (uc_reg_read(engine, UC_ARM64_REG_PC, &pc) != UC_ERR_OK) {
            return false;
        }
        pc = NormalizeCoordinateExecutionPointer(pc);
        if (!ContainsCoordinateExecutionCodeAddress(codeBase, codeSize, pc)) {
            return false;
        }

        std::uint32_t instruction = 0;
        if (uc_mem_read(engine, pc, &instruction, sizeof(instruction)) !=
                UC_ERR_OK ||
            !IsCoordinateExecutionTaggedMemoryInstruction(instruction)) {
            return false;
        }
        const std::uint32_t baseRegister =
            CoordinateExecutionMemoryBaseRegister(instruction);
        if (baseRegister == 31U) return false;

        std::uint64_t value = 0;
        if (!ReadXRegister(baseRegister, value)) return false;
        const std::uint64_t canonical =
            NormalizeCoordinateExecutionPointer(value);
        if (canonical == value ||
            !IsCoordinateExecutionCanonicalFaultBase(canonical) ||
            !WriteXRegister(baseRegister, canonical)) {
            return false;
        }
        ++evidence.taggedBaseRewriteCount;
        evidence.taggedBaseRegister = baseRegister;
        evidence.taggedBaseBefore = value;
        evidence.taggedBaseAfter = canonical;
        return true;
    }

    bool MapRemotePage(std::uint64_t faultAddress) {
        if (engine == nullptr || memory == nullptr) return false;
        const std::uint64_t guestPage = faultAddress & kPageMask;
        if (mappedPages.find(guestPage) != mappedPages.end()) return true;
        const std::uint64_t remotePage =
            NormalizeCoordinateExecutionPointer(faultAddress) & kPageMask;
        if (!IsCoordinateExecutionPointer(remotePage)) {
            probe.error = CoordinateExecutionRuntimeError::RemotePageReadFailed;
            return false;
        }

        auto backingIterator = pageBackings.find(remotePage);
        if (backingIterator == pageBackings.end()) {
            auto backing = std::make_shared<CachedPageBacking>();
            game::CoordinateReadDiagnostic diagnostic{};
            if (!ReadRemoteMemory(
                    remotePage,
                    backing->bytes.data(),
                    backing->bytes.size(),
                    diagnostic)) {
                probe.read = diagnostic;
                probe.error =
                    CoordinateExecutionRuntimeError::RemotePageReadFailed;
                return false;
            }
            backingIterator =
                pageBackings.emplace(remotePage, std::move(backing)).first;
        }

        const auto mapAlias = [&](std::uint64_t aliasPage) {
            if (mappedPages.find(aliasPage) != mappedPages.end()) return true;
            if (pages.size() >= kMaximumMappedPages) {
                probe.error =
                    CoordinateExecutionRuntimeError::GuestPageMapFailed;
                return false;
            }
            const std::shared_ptr<CachedPageBacking>& backing =
                backingIterator->second;
            CachedPage page{aliasPage, remotePage, backing, {}, {}};
            bool mapped = false;
            try {
                if (uc_mem_map_ptr(
                        engine,
                        aliasPage,
                        kPageSize,
                        UC_PROT_ALL,
                        backing->bytes.data()) != UC_ERR_OK) {
                    probe.error =
                        CoordinateExecutionRuntimeError::GuestPageMapFailed;
                    return false;
                }
                mapped = true;
                if (!InstallInstructionHooks(page, backing->bytes)) {
                    if (uc_mem_unmap(engine, aliasPage, kPageSize) !=
                        UC_ERR_OK) {
                        enginePoisoned = true;
                    }
                    return false;
                }
                const auto inserted =
                    pages.emplace(aliasPage, std::move(page));
                if (!inserted.second) {
                    enginePoisoned = true;
                    return false;
                }
                mappedPages.insert(aliasPage);
                return true;
            } catch (...) {
                enginePoisoned = true;
                auto tracked = pages.find(aliasPage);
                if (tracked != pages.end()) {
                    static_cast<void>(RemoveInstructionHooks(tracked->second));
                    pages.erase(tracked);
                } else {
                    static_cast<void>(RemoveInstructionHooks(page));
                }
                mappedPages.erase(aliasPage);
                if (mapped) {
                    static_cast<void>(
                        uc_mem_unmap(engine, aliasPage, kPageSize));
                }
                throw;
            }
        };

        if (!mapAlias(remotePage)) {
            return false;
        }
        if (guestPage != remotePage && !mapAlias(guestPage)) {
            probe.error = CoordinateExecutionRuntimeError::GuestPageMapFailed;
            return false;
        }
        return true;
    }

    bool ReadRemoteMemory(
        std::uint64_t address,
        void* destination,
        std::size_t size,
        game::CoordinateReadDiagnostic& diagnostic) {
        diagnostic = {};
        diagnostic.address = address;
        diagnostic.size = size;
        if (memory == nullptr) {
            diagnostic.failure =
                game::CoordinateReadFailure::TransportUnavailable;
            return false;
        }
        if (address > std::numeric_limits<std::uintptr_t>::max()) {
            diagnostic.failure = game::CoordinateReadFailure::InvalidRange;
            return false;
        }
        return memory->Read(
                   static_cast<std::uintptr_t>(address), destination, size) ||
            memory->ReadCoordinateMemory(
                   static_cast<std::uintptr_t>(address),
                   destination,
                   size,
                   diagnostic);
    }

    bool InstallInstructionHooks(
        CachedPage& page,
        const std::array<std::uint8_t, kPageSize>& bytes) {
        std::vector<std::uint64_t> addresses;
        addresses.reserve(128);
        const auto appendIfInPage = [&](std::uint64_t address) {
            if (address >= page.guestAddress &&
                address - page.guestAddress < kPageSize) {
                addresses.push_back(address);
            }
        };
        appendIfInPage(plan.hookPc);
        if (plan.returnStub != 0) appendIfInPage(plan.returnStub);
        const auto appendHookOffset = [&](std::uint64_t offset) {
            std::uint64_t address = 0;
            if (offset != 0 &&
                CoordinateExecutionAdd(plan.hookPc, offset, address)) {
                appendIfInPage(address);
            }
        };
        appendHookOffset(request.layout.hooks.subjectLoadInstruction);
        appendHookOffset(request.layout.hooks.callbackInstruction);
        appendHookOffset(request.layout.hooks.callbackReturn);
        const auto appendCallbackOffset = [&](std::uint64_t offset) {
            std::uint64_t address = 0;
            if (offset != 0 && evidence.callbackTarget != 0 &&
                CoordinateExecutionAdd(
                    NormalizeCoordinateExecutionPointer(
                        evidence.callbackTarget),
                    offset,
                    address)) {
                appendIfInPage(address);
            }
        };
        appendCallbackOffset(request.layout.hooks.callbackIndex);
        appendCallbackOffset(request.layout.hooks.callbackCopyPrepare);
        appendCallbackOffset(request.layout.hooks.callbackCopyAfter);
        const auto appendCodeOffset = [&](std::uint64_t offset) {
            std::uint64_t address = 0;
            if (offset != 0 &&
                CoordinateExecutionAdd(codeBase, offset, address)) {
                appendIfInPage(address);
            }
        };
        appendCodeOffset(request.layout.hooks.callbackTablePointerCode);
        appendCodeOffset(request.layout.hooks.callbackTableValueCode);
        appendCodeOffset(request.layout.hooks.callbackLockCode);
        appendCodeOffset(request.layout.hooks.callbackLockReturnCode);
        appendCodeOffset(request.layout.hooks.callbackFirstCallCode);
        appendCodeOffset(request.layout.hooks.callbackFirstReturnCode);
        appendCodeOffset(request.layout.hooks.callbackExternalCallCode);
        appendCodeOffset(request.layout.hooks.callbackExternalReturnCode);
        appendCodeOffset(request.layout.hooks.callbackPrimaryGateWriteCode);
        appendCodeOffset(request.layout.hooks.callbackAlternateGateWriteCode);
        appendCodeOffset(request.layout.hooks.callbackGateCode);
        appendCodeOffset(request.layout.hooks.callbackRecordCountCode);
        appendCodeOffset(request.layout.hooks.callbackTargetKeyCode);
        appendCodeOffset(request.layout.hooks.callbackRingSetupCode);
        appendCodeOffset(request.layout.hooks.callbackRingProbeCode);
        appendCodeOffset(request.layout.hooks.callbackRingHitCode);
        appendCodeOffset(request.layout.hooks.callbackDispatchCode);
        appendCodeOffset(request.layout.hooks.callbackDispatchReturnCode);
        appendCodeOffset(request.layout.hooks.callbackResultPrepareCode);
        appendCodeOffset(request.layout.hooks.callbackResultCode);
        const std::uint64_t remotePage =
            NormalizeCoordinateExecutionPointer(page.remoteAddress);
        const bool mainCodePage = remotePage >= codeBase &&
            remotePage - codeBase < codeSize;
        const std::uint64_t absoluteEntryPage =
            NormalizeCoordinateExecutionPointer(plan.entryPc) & kPageMask;
        const bool absoluteEntryCodePage =
            plan.entryPc != plan.hookPc &&
            remotePage >= absoluteEntryPage &&
            remotePage - absoluteEntryPage < kAbsoluteEntryWindowSize;
        const bool scanBranches = mainCodePage || absoluteEntryCodePage;
        for (std::size_t offset = 0; offset <= bytes.size() - 4; offset += 4) {
            std::uint32_t instruction = 0;
            std::memcpy(&instruction, bytes.data() + offset, sizeof(instruction));
            if ((instruction & kPacgaMask) == kPacgaOpcode ||
                (instruction & kSvcMask) == kSvcOpcode ||
                IsCoordinateExecutionLoadExclusiveInstruction(instruction) ||
                IsCoordinateExecutionStoreExclusiveInstruction(instruction) ||
                IsCoordinateExecutionClearExclusiveInstruction(instruction) ||
                (scanBranches &&
                 DecodeCoordinateExecutionBranch(instruction).IsValid())) {
                addresses.push_back(page.guestAddress + offset);
            }
        }
        std::sort(addresses.begin(), addresses.end());
        addresses.erase(
            std::unique(addresses.begin(), addresses.end()), addresses.end());

        for (const std::uint64_t address : addresses) {
            if (!hookedAddresses.insert(address).second) continue;
            uc_hook hook = 0;
            if (uc_hook_add(
                    engine,
                    &hook,
                    UC_HOOK_CODE,
                    reinterpret_cast<void*>(CodeHook),
                    this,
                    address,
                    address) != UC_ERR_OK) {
                hookedAddresses.erase(address);
                if (!RemoveInstructionHooks(page)) enginePoisoned = true;
                probe.error =
                    CoordinateExecutionRuntimeError::InstructionHookSetupFailed;
                return false;
            }
            page.instructionHooks.push_back(hook);
            page.instructionHookAddresses.push_back(address);
        }
        return true;
    }

    void HandleInstruction(std::uint64_t address) {
        if (address == plan.hookPc) {
            ++evidence.hookCount;
            if (ShouldInitializeCoordinateExecutionHook(
                    evidence.hookInitialized, address, plan.hookPc)) {
                std::uint64_t x0 = 0;
                std::uint64_t x1 = 0;
                std::uint64_t x2 = 0;
                if (uc_reg_read(engine, UC_ARM64_REG_X0, &x0) != UC_ERR_OK ||
                    uc_reg_read(engine, UC_ARM64_REG_X1, &x1) != UC_ERR_OK ||
                    uc_reg_read(engine, UC_ARM64_REG_X2, &x2) != UC_ERR_OK) {
                    FailHook(
                        address,
                        CoordinateExecutionRuntimeError::RegisterSetupFailed);
                    return;
                }
                evidence.hookX0 = x0;
                evidence.hookX1 = x1;
                evidence.hookX2 = x2;
                const std::uint64_t stackBase =
                    NormalizeCoordinateExecutionPointer(x1);
                if (!IsCoordinateExecutionStackBase(stackBase)) {
                    FailHook(
                        address,
                        CoordinateExecutionRuntimeError::EvidenceInvalid);
                    return;
                }
                std::uint64_t snapshotAddress = 0;
                if (CoordinateExecutionAdd(
                        stackBase,
                        request.layout.result.resultSlotOffset,
                        snapshotAddress)) {
                    static_cast<void>(uc_mem_read(
                        engine,
                        snapshotAddress,
                        &evidence.hookSlotValue,
                        sizeof(evidence.hookSlotValue)));
                }
                if (CoordinateExecutionAdd(
                        stackBase,
                        request.layout.result.resultSlotOffset + 8,
                        snapshotAddress)) {
                    static_cast<void>(uc_mem_read(
                        engine,
                        snapshotAddress,
                        &evidence.hookSnapshotX1,
                        sizeof(evidence.hookSnapshotX1)));
                }
                if (CoordinateExecutionAdd(
                        stackBase,
                        request.layout.result.resultSlotOffset + 0x10,
                        snapshotAddress)) {
                    static_cast<void>(uc_mem_read(
                        engine,
                        snapshotAddress,
                        &evidence.hookSnapshotX2,
                        sizeof(evidence.hookSnapshotX2)));
                }
                if (plan.seedSlotAtHook && !SeedResultSlot(stackBase)) {
                    FailHook(
                        address,
                        CoordinateExecutionRuntimeError::GuestPageWriteFailed);
                    return;
                }
                evidence.stackBase = stackBase;
                evidence.hookInitialized = true;
                evidence.hitHookPc = true;
            }
        }
        std::uint64_t probeAddress = 0;
        const auto matchesProbe = [&](std::uint64_t base,
                                      std::uint64_t offset) {
            return offset != 0 &&
                CoordinateExecutionAdd(base, offset, probeAddress) &&
                address == probeAddress;
        };
        const auto readDiagnosticField = [&](std::uint64_t base,
                                             std::uint64_t offset,
                                             void* output,
                                             std::size_t size) {
            std::uint64_t fieldAddress = 0;
            return offset != 0 &&
                CoordinateExecutionAdd(base, offset, fieldAddress) &&
                uc_mem_read(engine, fieldAddress, output, size) == UC_ERR_OK;
        };
        if (matchesProbe(
                plan.hookPc,
                request.layout.hooks.subjectLoadInstruction)) {
            ++evidence.subjectLoadCount;
            static_cast<void>(ReadXRegister(11, evidence.subjectLoadX11));
            static_cast<void>(ReadXRegister(9, evidence.subjectLoadAddress));
            static_cast<void>(uc_mem_read(
                engine,
                NormalizeCoordinateExecutionPointer(
                    evidence.subjectLoadAddress),
                &evidence.subjectLoadValue,
                sizeof(evidence.subjectLoadValue)));
        }
        if (matchesProbe(
                plan.hookPc,
                request.layout.hooks.callbackInstruction)) {
            ++evidence.callbackCount;
            static_cast<void>(ReadXRegister(0, evidence.callbackX0));
            static_cast<void>(ReadXRegister(1, evidence.callbackX1));
            static_cast<void>(ReadXRegister(2, evidence.callbackX2));
            static_cast<void>(ReadXRegister(8, evidence.callbackTarget));
            const std::uint64_t callbackX1 =
                NormalizeCoordinateExecutionPointer(evidence.callbackX1);
            std::uint64_t callbackX1End = 0;
            if (IsCoordinateExecutionPointer(callbackX1) &&
                CoordinateExecutionAdd(callbackX1, 0x18, callbackX1End) &&
                EnsureGuestRange(callbackX1, 0x18) &&
                uc_mem_read(
                    engine,
                    callbackX1,
                    &evidence.callbackX1Value0,
                    sizeof(evidence.callbackX1Value0)) == UC_ERR_OK &&
                uc_mem_read(
                    engine,
                    callbackX1 + 8,
                    &evidence.callbackX1Value8,
                    sizeof(evidence.callbackX1Value8)) == UC_ERR_OK &&
                uc_mem_read(
                    engine,
                    callbackX1 + 0x10,
                    &evidence.callbackX1Value10,
                    sizeof(evidence.callbackX1Value10)) == UC_ERR_OK) {
                evidence.callbackX1SnapshotValid = true;
            }
        }
        if (matchesProbe(
                plan.hookPc,
                request.layout.hooks.callbackReturn)) {
            ++evidence.callbackReturnCount;
            static_cast<void>(ReadXRegister(0, evidence.callbackReturnX0));
        }
        const std::uint64_t callbackTarget =
            NormalizeCoordinateExecutionPointer(evidence.callbackTarget);
        if (callbackTarget != 0 && matchesProbe(
                callbackTarget,
                request.layout.hooks.callbackIndex)) {
            ++evidence.callbackIndexCount;
            static_cast<void>(ReadXRegister(0, evidence.callbackIndex));
        }
        if (matchesProbe(
                codeBase,
                request.layout.hooks.callbackTablePointerCode)) {
            ++evidence.callbackTableProbeCount;
            static_cast<void>(
                ReadXRegister(8, evidence.callbackTablePointer));
            static_cast<void>(ReadXRegister(0, evidence.callbackTableIndex));
        }
        if (matchesProbe(
                codeBase,
                request.layout.hooks.callbackTableValueCode)) {
            static_cast<void>(ReadXRegister(8, evidence.callbackTableValue));
        }
        if (matchesProbe(
                codeBase,
                request.layout.hooks.callbackLockCode)) {
            static_cast<void>(
                ReadXRegister(0, evidence.callbackMutexRecord));
            static_cast<void>(ReadXRegister(9, evidence.callbackLockTarget));
            const std::uint64_t record = NormalizeCoordinateExecutionPointer(
                evidence.callbackMutexRecord);
            if (IsCoordinateExecutionPointer(record)) {
                static_cast<void>(uc_mem_read(
                    engine,
                    record,
                    &evidence.callbackMutexBeforeLock,
                    sizeof(evidence.callbackMutexBeforeLock)));
            }
        }
        if (matchesProbe(
                codeBase,
                request.layout.hooks.callbackLockReturnCode)) {
            static_cast<void>(ReadXRegister(0, evidence.callbackLockReturn));
            const std::uint64_t record = NormalizeCoordinateExecutionPointer(
                evidence.callbackMutexRecord);
            if (IsCoordinateExecutionPointer(record)) {
                static_cast<void>(uc_mem_read(
                    engine,
                    record,
                    &evidence.callbackMutexAfterLock,
                sizeof(evidence.callbackMutexAfterLock)));
            }
        }
        if (matchesProbe(
                codeBase,
                request.layout.hooks.callbackFirstCallCode)) {
            ++evidence.callbackFirstCallCount;
            static_cast<void>(
                ReadXRegister(8, evidence.callbackFirstTarget));
            static_cast<void>(
                ReadXRegister(0, evidence.callbackFirstArgument));
        }
        if (matchesProbe(
                codeBase,
                request.layout.hooks.callbackFirstReturnCode)) {
            ++evidence.callbackFirstReturnCount;
            static_cast<void>(
                ReadXRegister(0, evidence.callbackFirstReturn));
        }
        if (matchesProbe(
                codeBase,
                request.layout.hooks.callbackExternalCallCode)) {
            ++evidence.callbackExternalCallCount;
            static_cast<void>(
                ReadXRegister(8, evidence.callbackExternalTarget));
            static_cast<void>(ReadXRegister(0, evidence.callbackExternalX0));
            static_cast<void>(ReadXRegister(1, evidence.callbackExternalX1));
            static_cast<void>(ReadXRegister(2, evidence.callbackExternalX2));
            static_cast<void>(ReadXRegister(3, evidence.callbackExternalX3));
        }
        if (matchesProbe(
                codeBase,
                request.layout.hooks.callbackExternalReturnCode)) {
            ++evidence.callbackExternalReturnCount;
            static_cast<void>(
                ReadXRegister(0, evidence.callbackExternalReturn));
            std::uint64_t context = 0;
            if (ReadXRegister(21, context)) {
                context = NormalizeCoordinateExecutionPointer(context);
                if (IsCoordinateExecutionPointer(context)) {
                    static_cast<void>(readDiagnosticField(
                        context,
                        request.layout.fields.externalExpected,
                        &evidence.callbackExternalExpected,
                        sizeof(evidence.callbackExternalExpected)));
                }
            }
            std::uint64_t sp = 0;
            if (uc_reg_read(engine, UC_ARM64_REG_SP, &sp) == UC_ERR_OK) {
                static_cast<void>(readDiagnosticField(
                    sp,
                    request.layout.fields.externalPriorGate,
                    &evidence.callbackExternalPriorGate,
                    sizeof(evidence.callbackExternalPriorGate)));
            }
        }
        if (matchesProbe(
                codeBase,
                request.layout.hooks.callbackPrimaryGateWriteCode)) {
            ++evidence.callbackPrimaryGateWriteCount;
            std::uint64_t value = 0;
            if (ReadXRegister(8, value)) {
                evidence.callbackPrimaryGateWriteValue =
                    static_cast<std::uint32_t>(value);
            }
            std::uint64_t sp = 0;
            if (uc_reg_read(engine, UC_ARM64_REG_SP, &sp) == UC_ERR_OK) {
                static_cast<void>(readDiagnosticField(
                    sp,
                    request.layout.fields.primaryGateSource,
                    &evidence.callbackPrimaryGateSource,
                    sizeof(evidence.callbackPrimaryGateSource)));
            }
        }
        if (matchesProbe(
                codeBase,
                request.layout.hooks.callbackAlternateGateWriteCode)) {
            ++evidence.callbackAlternateGateWriteCount;
            std::uint64_t value = 0;
            if (ReadXRegister(8, value)) {
                evidence.callbackAlternateGateWriteValue =
                    static_cast<std::uint32_t>(value);
            }
        }
        if (matchesProbe(
                codeBase,
                request.layout.hooks.callbackGateCode)) {
            ++evidence.callbackGateProbeCount;
            std::uint64_t state = 0;
            if (ReadXRegister(9, state)) {
                evidence.callbackGateState =
                    static_cast<std::uint32_t>(state);
            }
            std::uint64_t sp = 0;
            if (uc_reg_read(engine, UC_ARM64_REG_SP, &sp) == UC_ERR_OK) {
                static_cast<void>(readDiagnosticField(
                    sp,
                    request.layout.fields.gateFlag,
                    &evidence.callbackGateFlag,
                    sizeof(evidence.callbackGateFlag)));
                static_cast<void>(readDiagnosticField(
                    sp,
                    request.layout.fields.gateSnapshotA,
                    &evidence.callbackGateSnapshotA,
                    sizeof(evidence.callbackGateSnapshotA)));
                static_cast<void>(readDiagnosticField(
                    sp,
                    request.layout.fields.gateSnapshotB,
                    &evidence.callbackGateSnapshotB,
                    sizeof(evidence.callbackGateSnapshotB)));
            }
        }
        if (matchesProbe(
                codeBase,
                request.layout.hooks.callbackRecordCountCode)) {
            std::uint64_t count = 0;
            if (ReadXRegister(10, count)) {
                evidence.callbackRecordCount =
                    static_cast<std::uint32_t>(count);
            }
        }
        if (matchesProbe(
                codeBase,
                request.layout.hooks.callbackTargetKeyCode)) {
            static_cast<void>(ReadXRegister(8, evidence.callbackTargetKey));
        }
        if (matchesProbe(
                codeBase,
                request.layout.hooks.callbackRingSetupCode)) {
            static_cast<void>(ReadXRegister(10, evidence.callbackRingBase));
            static_cast<void>(
                ReadXRegister(8, evidence.callbackRingIndexArray));
        }
        if (matchesProbe(
                codeBase,
                request.layout.hooks.callbackRingProbeCode)) {
            ++evidence.callbackRingProbeCount;
            static_cast<void>(ReadXRegister(9, evidence.callbackRingRowKey));
            std::uint64_t rowIndex = 0;
            if (ReadXRegister(8, rowIndex)) {
                evidence.callbackRingRowIndex =
                    static_cast<std::int64_t>(rowIndex);
            }
            std::uint64_t sp = 0;
            if (uc_reg_read(engine, UC_ARM64_REG_SP, &sp) == UC_ERR_OK) {
                static_cast<void>(readDiagnosticField(
                    sp,
                    request.layout.fields.ringMid,
                    &evidence.callbackRingMid,
                    sizeof(evidence.callbackRingMid)));
            }
        }
        if (matchesProbe(
                codeBase,
                request.layout.hooks.callbackRingHitCode)) {
            ++evidence.callbackRingHitCount;
            static_cast<void>(ReadXRegister(22, evidence.callbackRingHitRow));
        }
        if (matchesProbe(
                codeBase,
                request.layout.hooks.callbackDispatchCode)) {
            ++evidence.callbackDispatchCount;
            static_cast<void>(
                ReadXRegister(8, evidence.callbackDispatchTarget));
            static_cast<void>(
                ReadXRegister(0, evidence.callbackDispatchArgument));
            static_cast<void>(
                ReadCallbackPoolIndex(evidence.callbackPoolIndexBefore));
            const std::uint64_t record = NormalizeCoordinateExecutionPointer(
                evidence.callbackMutexRecord);
            if (IsCoordinateExecutionPointer(record)) {
                static_cast<void>(uc_mem_read(
                    engine,
                    record,
                    &evidence.callbackMutexBeforeUnlock,
                    sizeof(evidence.callbackMutexBeforeUnlock)));
            }
        }
        if (matchesProbe(
                codeBase,
                request.layout.hooks.callbackDispatchReturnCode)) {
            ++evidence.callbackDispatchReturnCount;
            static_cast<void>(
                ReadXRegister(0, evidence.callbackDispatchReturn));
            static_cast<void>(
                ReadCallbackPoolIndex(evidence.callbackPoolIndexAfter));
            const std::uint64_t record = NormalizeCoordinateExecutionPointer(
                evidence.callbackMutexRecord);
            if (IsCoordinateExecutionPointer(record)) {
                static_cast<void>(uc_mem_read(
                    engine,
                    record,
                    &evidence.callbackMutexAfterUnlock,
                    sizeof(evidence.callbackMutexAfterUnlock)));
            }
        }
        if (matchesProbe(
                codeBase,
                request.layout.hooks.callbackResultPrepareCode)) {
            ++evidence.callbackResultCount;
            static_cast<void>(
                ReadXRegister(8, evidence.callbackResultIndex));
            static_cast<void>(
                ReadXRegister(9, evidence.callbackResultBase));
        }
        if (matchesProbe(
                codeBase,
                request.layout.hooks.callbackResultCode)) {
            static_cast<void>(
                ReadXRegister(8, evidence.callbackResultPointer));
            evidence.callbackResultPositionValid = CapturePositionBits(
                evidence.callbackResultPointer,
                request.layout.fields.resultPosition,
                evidence.callbackResultPositionX,
                evidence.callbackResultPositionY,
                evidence.callbackResultPositionZ);
        }
        if (callbackTarget != 0 && matchesProbe(
                callbackTarget,
                request.layout.hooks.callbackCopyPrepare)) {
            ++evidence.callbackCopyPrepareCount;
            static_cast<void>(
                ReadXRegister(13, evidence.callbackCopySource));
            static_cast<void>(
                ReadXRegister(11, evidence.callbackCopyDestination));
            const std::uint64_t source = NormalizeCoordinateExecutionPointer(
                evidence.callbackCopySource);
            if (IsCoordinateExecutionPointer(source) &&
                EnsureGuestRange(source, 0x20)) {
                static_cast<void>(uc_mem_read(
                    engine,
                    source,
                    &evidence.callbackCopySourceValue0,
                    sizeof(evidence.callbackCopySourceValue0)));
                static_cast<void>(uc_mem_read(
                    engine,
                    source + 8,
                    &evidence.callbackCopySourceValue8,
                    sizeof(evidence.callbackCopySourceValue8)));
                static_cast<void>(uc_mem_read(
                    engine,
                    source + 0x10,
                    &evidence.callbackCopySourceValue10,
                    sizeof(evidence.callbackCopySourceValue10)));
                static_cast<void>(uc_mem_read(
                    engine,
                    source + 0x18,
                    &evidence.callbackCopySourceValue18,
                    sizeof(evidence.callbackCopySourceValue18)));
            }
        }
        if (callbackTarget != 0 && matchesProbe(
                callbackTarget,
                request.layout.hooks.callbackCopyAfter)) {
            ++evidence.callbackCopyAfterCount;
            const std::uint64_t destination =
                NormalizeCoordinateExecutionPointer(
                    evidence.callbackCopyDestination);
            if (IsCoordinateExecutionPointer(destination) &&
                EnsureGuestRange(destination, 0x20)) {
                static_cast<void>(uc_mem_read(
                    engine,
                    destination,
                    &evidence.callbackCopyDestinationValue0,
                    sizeof(evidence.callbackCopyDestinationValue0)));
                static_cast<void>(uc_mem_read(
                    engine,
                    destination + 8,
                    &evidence.callbackCopyDestinationValue8,
                    sizeof(evidence.callbackCopyDestinationValue8)));
                static_cast<void>(uc_mem_read(
                    engine,
                    destination + 0x10,
                    &evidence.callbackCopyDestinationValue10,
                    sizeof(evidence.callbackCopyDestinationValue10)));
                static_cast<void>(uc_mem_read(
                    engine,
                    destination + 0x18,
                    &evidence.callbackCopyDestinationValue18,
                    sizeof(evidence.callbackCopyDestinationValue18)));
            }
        }
        if (ShouldRedirectCoordinateExecutionReturn(plan, address)) {
            evidence.hitReturnStub = true;
            if (uc_reg_write(
                    engine,
                    UC_ARM64_REG_PC,
                    &kCoordinateExecutionStopPc) != UC_ERR_OK ||
                uc_emu_stop(engine) != UC_ERR_OK) {
                FailHook(
                    address,
                    CoordinateExecutionRuntimeError::EmulationFailed);
            }
            return;
        }

        std::uint32_t instruction = 0;
        if (uc_mem_read(
                engine, address, &instruction, sizeof(instruction)) !=
            UC_ERR_OK) {
            FailHook(address, CoordinateExecutionRuntimeError::EmulationFailed);
            return;
        }
        if (IsCoordinateExecutionLoadExclusiveInstruction(instruction)) {
            ++evidence.exclusiveLoadCount;
            evidence.lastExclusiveInstruction = instruction;
            exclusiveMonitorInvalid =
                CoordinateExecutionExclusiveMonitorInvalidAfterInstruction(
                    exclusiveMonitorInvalid, instruction);
        } else if (IsCoordinateExecutionClearExclusiveInstruction(
                       instruction)) {
            ++evidence.exclusiveClearCount;
            evidence.lastExclusiveInstruction = instruction;
            exclusiveMonitorInvalid =
                CoordinateExecutionExclusiveMonitorInvalidAfterInstruction(
                    exclusiveMonitorInvalid, instruction);
        } else if (IsCoordinateExecutionStoreExclusiveInstruction(
                       instruction)) {
            ++evidence.exclusiveStoreCount;
            evidence.lastExclusiveInstruction = instruction;
            const std::uint32_t statusRegister =
                CoordinateExecutionStoreExclusiveStatusRegister(instruction);
            evidence.exclusiveStoreStatusRegister = statusRegister;
            if (exclusiveMonitorInvalid) {
                exclusiveMonitorInvalid =
                    CoordinateExecutionExclusiveMonitorInvalidAfterInstruction(
                        exclusiveMonitorInvalid, instruction);
                ++evidence.exclusiveStoreFailureCount;
                if (!WriteXRegister(statusRegister, 1) ||
                    !SkipInstruction(address)) {
                    FailHook(
                        address,
                        CoordinateExecutionRuntimeError::EmulationFailed);
                }
                return;
            }
            exclusiveMonitorInvalid =
                CoordinateExecutionExclusiveMonitorInvalidAfterInstruction(
                    exclusiveMonitorInvalid, instruction);
        } else if ((instruction & kPacgaMask) == kPacgaOpcode) {
            HandlePacga(address, instruction);
        } else if ((instruction & kSvcMask) == kSvcOpcode) {
            HandleSvc(address);
        } else {
            const CoordinateExecutionBranch branch =
                DecodeCoordinateExecutionBranch(instruction);
            if (branch.IsValid()) {
                HandleBranch(address, instruction, branch);
            }
        }
    }

    void HandlePacga(std::uint64_t address,
                     std::uint32_t instruction) {
        const std::uint32_t destination = instruction & 0x1FU;
        const std::uint32_t source = (instruction >> 5U) & 0x1FU;
        const std::uint32_t modifier = (instruction >> 16U) & 0x1FU;
        std::uint64_t sourceValue = 0;
        std::uint64_t modifierValue = 0;
        if (!ReadXRegister(source, sourceValue) ||
            !ReadXRegister(modifier, modifierValue)) {
            FailHook(address, CoordinateExecutionRuntimeError::EmulationFailed);
            return;
        }

        std::uint64_t result = 0;
        if (!FindPacgaOracle(sourceValue, modifierValue, result) &&
            executionContext.HasPacgaKey()) {
            result = ComputeArm64Pacga(
                sourceValue,
                modifierValue,
                Arm64PacgaKey{
                    executionContext.pacgaLow,
                    executionContext.pacgaHigh,
                });
            CachePacgaOracle(sourceValue, modifierValue, result);
        } else if (!FindPacgaOracle(
                       sourceValue, modifierValue, result)) {
            std::uint64_t tpidrEl0 = 0;
            const PacgaOracleInstruction oracle{
                static_cast<std::uintptr_t>(address),
                sourceValue,
                modifierValue,
                instruction,
                PacgaOracleCodeIdentity{
                    static_cast<std::uintptr_t>(address),
                    instruction,
                },
            };
            if (pacgaOracleReader.Read(
                    executionContext.threadId,
                    oracle,
                    tpidrEl0,
                    result) != 0 ||
                tpidrEl0 != executionContext.tpidrEl0) {
                FailHook(
                    address,
                    CoordinateExecutionRuntimeError::PacgaUnavailable);
                return;
            }
            CachePacgaOracle(sourceValue, modifierValue, result);
        }
        if ((result & UINT64_C(0xFFFFFFFF)) != 0) {
            FailHook(
                address,
                CoordinateExecutionRuntimeError::PacgaUnavailable);
            return;
        }
        ++evidence.pacgaCount;
        evidence.lastPacgaSource = sourceValue;
        evidence.lastPacgaModifier = modifierValue;
        evidence.lastPacgaResult = result;
        if (!WriteXRegister(destination, result) || !SkipInstruction(address)) {
            FailHook(address, CoordinateExecutionRuntimeError::EmulationFailed);
        }
    }

    void HandleSvc(std::uint64_t address) {
        std::uint64_t number = 0;
        if (!ReadXRegister(8, number)) {
            FailHook(address, CoordinateExecutionRuntimeError::UnsupportedSvc);
            return;
        }
        switch (evidence.svcCount) {
            case 0: evidence.svcNumber0 = number; break;
            case 1: evidence.svcNumber1 = number; break;
            case 2: evidence.svcNumber2 = number; break;
            case 3: evidence.svcNumber3 = number; break;
            default: break;
        }
        ++evidence.svcCount;
        evidence.lastSvcNumber = number;
        ++evidence.exclusiveClearCount;
        exclusiveMonitorInvalid = true;
        if (!WriteXRegister(0, CoordinateExecutionSvcResult(number)) ||
            !SkipInstruction(address)) {
            FailHook(address, CoordinateExecutionRuntimeError::EmulationFailed);
        }
    }

    bool IsMainCodeTarget(std::uint64_t target) const noexcept {
        target = NormalizeCoordinateExecutionPointer(target);
        return target >= codeBase && target - codeBase < codeSize;
    }

    bool IsLibcTarget(std::uint64_t target) const noexcept {
        target = NormalizeCoordinateExecutionPointer(target);
        return libcBegin != 0 && libcEnd > libcBegin &&
            target >= libcBegin && target < libcEnd;
    }

    bool IsAbsoluteEntryAddress(std::uint64_t address) const noexcept {
        if (plan.entryPc == plan.hookPc) return false;
        const std::uint64_t begin =
            NormalizeCoordinateExecutionPointer(plan.entryPc) & kPageMask;
        address = NormalizeCoordinateExecutionPointer(address);
        return address >= begin &&
            address - begin < kAbsoluteEntryWindowSize;
    }

    bool ResolveBranchTarget(
        std::uint64_t address,
        std::uint32_t instruction,
        const CoordinateExecutionBranch& branch,
        std::uint64_t& target) const {
        if (branch.immediate) {
            target = ResolveCoordinateExecutionImmediateBranchTarget(
                address, instruction);
            return true;
        }
        return ReadXRegister(branch.targetRegister, target) &&
            (target = NormalizeCoordinateExecutionPointer(target), true);
    }

    bool CompleteExternalCall(std::uint64_t returnPc,
                              bool call,
                              std::uint64_t result) {
        evidence.lastExternalBranchReturnPc = returnPc;
        evidence.lastExternalBranchResult = result;
        return WriteXRegister(0, result) &&
            (!call || WriteXRegister(30, returnPc)) &&
            uc_reg_write(engine, UC_ARM64_REG_PC, &returnPc) == UC_ERR_OK;
    }

    std::uint64_t AllocateFakeDescriptor() {
        const std::uint32_t descriptor = nextFakeDescriptor++;
        fakeFiles[static_cast<std::int32_t>(descriptor)] = {};
        ++evidence.fakeDescriptorCount;
        return descriptor;
    }

    bool TryHandleDescriptorEndQuery(std::uint64_t returnPc) {
        std::uint64_t number = 0;
        std::uint64_t descriptorValue = 0;
        std::uint64_t offset = 0;
        std::uint64_t whence = 0;
        if (!ReadXRegister(0, number) ||
            !ReadXRegister(1, descriptorValue) ||
            !ReadXRegister(2, offset) ||
            !ReadXRegister(3, whence) ||
            !IsCoordinateExecutionDescriptorEndQuery(
                number, offset, whence)) {
            return false;
        }

        const std::int32_t descriptor =
            static_cast<std::int32_t>(descriptorValue);
        const auto file = fakeFiles.find(descriptor);
        if (file == fakeFiles.end()) return false;
        const CoordinateExecutionDescriptorSeekResult seek =
            EvaluateCoordinateExecutionDescriptorSeek(
                file->second.end,
                file->second.current,
                std::bit_cast<std::int64_t>(offset),
                static_cast<std::int32_t>(whence));
        file->second.current = seek.current;
        emptyFakeFileSeen = emptyFakeFileSeen || seek.emptyFile;
        ++evidence.descriptorEndQueryCount;
        evidence.descriptorEndQueryFd = descriptor;
        evidence.descriptorEndQueryResult = seek.result;
        return CompleteExternalCall(
            returnPc, true, static_cast<std::uint64_t>(seek.result));
    }

    bool IsAllowedExternalJumpReturn(std::uint64_t target) const noexcept {
        target = NormalizeCoordinateExecutionPointer(target);
        return target == kCoordinateExecutionStopPc ||
            ShouldRedirectCoordinateExecutionReturn(plan, target) ||
            IsMainCodeTarget(target) || IsAbsoluteEntryAddress(target) ||
            IsLibcTarget(target);
    }

    void HandleBranch(
        std::uint64_t address,
        std::uint32_t instruction,
        const CoordinateExecutionBranch& branch) {
        std::uint64_t target = 0;
        if (address > std::numeric_limits<std::uint64_t>::max() - 4 ||
            !ResolveBranchTarget(address, instruction, branch, target)) {
            FailHook(address, CoordinateExecutionRuntimeError::EmulationFailed);
            return;
        }
        const std::uint64_t returnPc = address + 4;
        if (target == kCoordinateExecutionStopPc ||
            ShouldRedirectCoordinateExecutionReturn(plan, target) ||
            IsMainCodeTarget(target)) {
            return;
        }

        ++evidence.externalBranchCount;
        evidence.lastExternalBranchTarget = target;
        if (IsLibcTarget(target)) {
            ++evidence.libcBranchCount;
            if (branch.IsCall() &&
                TryHandleDescriptorEndQuery(returnPc)) {
                return;
            }
            if (!CompleteExternalCall(returnPc, branch.IsCall(), 0)) {
                FailHook(
                    address,
                    CoordinateExecutionRuntimeError::EmulationFailed);
            }
            return;
        }

        if (branch.IsCall()) {
            ++evidence.unknownExternalCallCount;
            const std::uint64_t result = IsAbsoluteEntryAddress(address)
                ? 0
                : AllocateFakeDescriptor();
            if (!CompleteExternalCall(returnPc, true, result)) {
                FailHook(
                    address,
                    CoordinateExecutionRuntimeError::EmulationFailed);
            }
            return;
        }

        ++evidence.unknownExternalJumpCount;
        std::uint64_t link = 0;
        const std::uint64_t result = IsAbsoluteEntryAddress(address) ? 0 : 1;
        if (!ReadXRegister(30, link) || !WriteXRegister(0, result)) {
            FailHook(address, CoordinateExecutionRuntimeError::EmulationFailed);
            return;
        }
        link = NormalizeCoordinateExecutionPointer(link);
        const std::uint64_t next = IsAllowedExternalJumpReturn(link)
            ? link
            : kCoordinateExecutionStopPc;
        evidence.lastExternalBranchReturnPc = next;
        evidence.lastExternalBranchResult = result;
        if (uc_reg_write(engine, UC_ARM64_REG_PC, &next) != UC_ERR_OK) {
            FailHook(address, CoordinateExecutionRuntimeError::EmulationFailed);
        }
    }

    void CaptureWrite(std::uint64_t address,
                      int size,
                      std::int64_t rawValue) noexcept {
        const std::uint64_t stackBase = evidence.stackBase != 0
            ? evidence.stackBase
            : NormalizeCoordinateExecutionPointer(plan.expectedStackBase);
        std::uint64_t slot = 0;
        if (size != static_cast<int>(sizeof(std::uint64_t)) ||
            stackBase == 0 ||
            !CoordinateExecutionAdd(
                stackBase, request.layout.result.resultSlotOffset, slot)) {
            return;
        }
        slot = NormalizeCoordinateExecutionPointer(slot);
        const std::uint64_t object = NormalizeCoordinateExecutionPointer(
            static_cast<std::uint64_t>(rawValue));
        if (NormalizeCoordinateExecutionPointer(address) != slot ||
            !IsCoordinateExecutionPointer(object) || object == subject) {
            return;
        }

        std::uint64_t pc = 0;
        if (uc_reg_read(engine, UC_ARM64_REG_PC, &pc) != UC_ERR_OK ||
            uc_reg_read(engine, UC_ARM64_REG_SP, &evidence.capturedSp) !=
                UC_ERR_OK ||
            !ReadXRegister(8, evidence.capturedX8) ||
            !ReadXRegister(9, evidence.capturedX9) ||
            !ReadXRegister(12, evidence.capturedX12) ||
            !ReadXRegister(21, evidence.capturedX21)) {
            return;
        }
        const auto readLocal = [&](std::uint64_t offset,
                                   std::uint64_t& value) {
            std::uint64_t localAddress = 0;
            return offset != 0 && CoordinateExecutionAdd(
                       evidence.capturedSp, offset, localAddress) &&
                uc_mem_read(engine, localAddress, &value, sizeof(value)) ==
                UC_ERR_OK;
        };
        static_cast<void>(readLocal(
            request.layout.fields.capturedLocal0,
            evidence.capturedLocal0));
        static_cast<void>(readLocal(
            request.layout.fields.capturedLocal1,
            evidence.capturedLocal1));
        static_cast<void>(readLocal(
            request.layout.fields.capturedLocal2,
            evidence.capturedLocal2));
        static_cast<void>(readLocal(
            request.layout.fields.capturedLocal3,
            evidence.capturedLocal3));
        std::uint64_t fieldAddress = 0;
        if (request.layout.fields.capturedLocalField != 0 &&
            CoordinateExecutionAdd(
                NormalizeCoordinateExecutionPointer(
                    evidence.capturedLocal3),
                request.layout.fields.capturedLocalField,
                fieldAddress)) {
            static_cast<void>(uc_mem_read(
                engine,
                fieldAddress,
                &evidence.capturedLocalField,
                sizeof(evidence.capturedLocalField)));
        }
        evidence.captureValid = true;
        ++evidence.captureCount;
        evidence.capturedPc = pc;
        evidence.capturedSlot = slot;
        evidence.capturedObject = object;
        evidence.capturedWriteSize = sizeof(std::uint64_t);
    }

    CoordinateExecutionResult ValidateResult() {
        if (!evidence.hitStopPc || !evidence.captureValid ||
            evidence.captureCount == 0 ||
            evidence.capturedWriteSize != sizeof(std::uint64_t) ||
            (plan.requireReturnStub && !evidence.hitReturnStub) ||
            (plan.verifyReturnStubMagic &&
             !evidence.returnStubMagicVerifiedBeforeRun)) {
            probe.error = CoordinateExecutionRuntimeError::EvidenceInvalid;
            return Failure(CoordinateExecutionStatus::EvidenceFailure);
        }

        const std::uint64_t stackBase = evidence.stackBase != 0
            ? NormalizeCoordinateExecutionPointer(evidence.stackBase)
            : NormalizeCoordinateExecutionPointer(plan.expectedStackBase);
        std::uint64_t slot = 0;
        if (stackBase == 0 ||
            !CoordinateExecutionAdd(
                stackBase, request.layout.result.resultSlotOffset, slot)) {
            probe.error = CoordinateExecutionRuntimeError::EvidenceInvalid;
            return Failure(CoordinateExecutionStatus::EvidenceFailure);
        }
        slot = NormalizeCoordinateExecutionPointer(slot);

        std::uint64_t slotValue = 0;
        if (uc_mem_read(engine, slot, &slotValue, sizeof(slotValue)) !=
            UC_ERR_OK) {
            probe.error = CoordinateExecutionRuntimeError::ResultReadFailed;
            return Failure(CoordinateExecutionStatus::EvidenceFailure);
        }
        const std::uint64_t object =
            NormalizeCoordinateExecutionPointer(slotValue);
        if (!IsCoordinateExecutionPointer(object) || object == subject ||
            NormalizeCoordinateExecutionPointer(evidence.capturedObject) !=
                object ||
            NormalizeCoordinateExecutionPointer(evidence.capturedSlot) !=
                slot ||
            !ContainsCoordinateExecutionCodeAddress(
                codeBase, codeSize, evidence.capturedPc)) {
            probe.error = CoordinateExecutionRuntimeError::ResultInvalid;
            return Failure(CoordinateExecutionStatus::EvidenceFailure);
        }

        std::uint64_t positionAddress = 0;
        if (!CoordinateExecutionAdd(
                object,
                request.layout.result.positionOffset,
                positionAddress) ||
            (request.mode == CoordinateExecutionMode::Emulate &&
             !IsCoordinateExecutionPointer(positionAddress)) ||
            !EnsureGuestRange(
                positionAddress, sizeof(CoordinateExecutionPosition))) {
            probe.error = CoordinateExecutionRuntimeError::ResultReadFailed;
            return Failure(
                CoordinateExecutionStatus::ReadOrCoordinateFailure);
        }
        CoordinateExecutionPosition position{};
        if (uc_mem_read(
                engine,
                positionAddress,
                &position,
                sizeof(position)) != UC_ERR_OK ||
            !IsFiniteCoordinate(position)) {
            probe.error = CoordinateExecutionRuntimeError::ResultInvalid;
            return Failure(
                CoordinateExecutionStatus::ReadOrCoordinateFailure);
        }
        return Success(position, object);
    }

    bool ReadXRegister(std::uint32_t index, std::uint64_t& value) const {
        value = 0;
        return index == 31 ||
            uc_reg_read(engine, XRegisterId(index), &value) == UC_ERR_OK;
    }

    bool CapturePositionBits(std::uint64_t base,
                             std::uint64_t offset,
                             std::uint32_t& x,
                             std::uint32_t& y,
                             std::uint32_t& z) const {
        base = NormalizeCoordinateExecutionPointer(base);
        std::uint64_t address = 0;
        CoordinateExecutionPosition position{};
        if (offset == 0 || !IsCoordinateExecutionPointer(base) ||
            !CoordinateExecutionAdd(base, offset, address) ||
            uc_mem_read(engine, address, &position, sizeof(position)) !=
                UC_ERR_OK) {
            return false;
        }
        x = std::bit_cast<std::uint32_t>(position.x);
        y = std::bit_cast<std::uint32_t>(position.y);
        z = std::bit_cast<std::uint32_t>(position.z);
        return true;
    }

    bool ReadCallbackPoolIndex(std::uint64_t& value) const {
        value = 0;
        std::uint64_t sp = 0;
        std::uint64_t context = 0;
        std::uint64_t selector = 0;
        if (request.layout.fields.poolSelector == 0 ||
            request.layout.fields.poolTable == 0 ||
            uc_reg_read(engine, UC_ARM64_REG_SP, &sp) != UC_ERR_OK ||
            !ReadXRegister(21, context) ||
            uc_mem_read(
                engine,
                sp + request.layout.fields.poolSelector,
                &selector,
                sizeof(selector)) != UC_ERR_OK ||
            selector >
                (std::numeric_limits<std::uint64_t>::max() - context) / 8) {
            return false;
        }
        std::uint64_t slot = context + selector * 8;
        std::uint64_t pointer = 0;
        if (!CoordinateExecutionAdd(
                slot, request.layout.fields.poolTable, slot) ||
            uc_mem_read(engine, slot, &pointer, sizeof(pointer)) != UC_ERR_OK) {
            return false;
        }
        pointer = NormalizeCoordinateExecutionPointer(pointer);
        std::int32_t index = 0;
        if (!IsCoordinateExecutionPointer(pointer) ||
            uc_mem_read(engine, pointer, &index, sizeof(index)) != UC_ERR_OK) {
            return false;
        }
        value = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(index));
        return true;
    }

    bool WriteXRegister(std::uint32_t index, std::uint64_t value) {
        return index == 31 ||
            uc_reg_write(engine, XRegisterId(index), &value) == UC_ERR_OK;
    }

    bool SkipInstruction(std::uint64_t address) {
        if (address > std::numeric_limits<std::uint64_t>::max() - 4) {
            return false;
        }
        const std::uint64_t next = address + 4;
        return uc_reg_write(engine, UC_ARM64_REG_PC, &next) == UC_ERR_OK;
    }

    void FailHook(std::uint64_t address,
                  CoordinateExecutionRuntimeError error) noexcept {
        hookFailed = true;
        hookRuntimeError = error;
        if (address != 0) probe.faultAddress = address;
        if (engine != nullptr) static_cast<void>(uc_emu_stop(engine));
    }

    mutable std::mutex mutex;
    uc_engine* engine = nullptr;
    uc_hook memoryFaultHook = 0;
    uc_hook memoryWriteHook = 0;
    uc_hook mrsHook = 0;
    MemoryTransport* memory = nullptr;
    std::uint64_t codeBase = 0;
    std::size_t codeSize = 0;
    std::uint64_t subject = 0;
    ProcessExecutionContext executionContext{};
    CoordinateExecutionRequest request{};
    CoordinateExecutionPlan plan{};
    CoordinateExecutionEvidence evidence{};
    CoordinateExecutionRuntimeProbe probe{};
    std::uint64_t ctrEl0 = 0;
    bool ctrEl0Initialized = false;
    std::uint64_t counterFrequency = 0;
    bool hookFailed = false;
    CoordinateExecutionRuntimeError hookRuntimeError =
        CoordinateExecutionRuntimeError::None;
    std::unordered_map<std::uint64_t, CachedPage> pages;
    std::unordered_map<
        std::uint64_t,
        std::shared_ptr<CachedPageBacking>> pageBackings;
    std::unordered_set<std::uint64_t> mappedPages;
    std::unordered_set<std::uint64_t> hookedAddresses;
    std::unordered_set<std::uint64_t> dirtyStackPages;
    MemoryTransport* engineMemory = nullptr;
    std::uintptr_t engineModuleBase = 0;
    std::size_t engineModuleSize = 0;
    std::uintptr_t engineCodeBase = 0;
    std::size_t engineCodeSize = 0;
    CoordinateExecutionMode engineMode =
        static_cast<CoordinateExecutionMode>(0);
    CoordinateExecutionLayout engineLayout{};
    CoordinateExecutionPlan enginePlan{};
    bool engineIdentityValid = false;
    bool exclusiveMonitorInvalid = true;
    bool enginePoisoned = false;
    std::int32_t libcThreadId = -1;
    std::uint64_t libcThreadStartTimeTicks = 0;
    std::uint64_t libcBegin = 0;
    std::uint64_t libcEnd = 0;
    std::uint32_t nextFakeDescriptor = kFirstFakeDescriptor;
    std::unordered_map<std::int32_t, FakeFile> fakeFiles;
    bool emptyFakeFileSeen = false;
    PtracePacgaOracleReader pacgaOracleReader;
    std::int32_t pacgaThreadId = -1;
    std::uint64_t pacgaThreadStartTimeTicks = 0;
    std::uint64_t pacgaTpidrEl0 = 0;
    std::vector<CachedPacgaOracle> pacgaOracles;
};

CoordinateExecutionRuntime::CoordinateExecutionRuntime()
    : impl_(std::make_unique<Impl>()) {}

CoordinateExecutionRuntime::~CoordinateExecutionRuntime() = default;

CoordinateExecutionResult CoordinateExecutionRuntime::Execute(
    MemoryTransport& memory,
    std::uintptr_t moduleBase,
    std::size_t moduleSize,
    std::uintptr_t codeBase,
    std::size_t codeSize,
    std::uintptr_t subject,
    const ProcessExecutionContext& executionContext,
    const CoordinateExecutionRequest& request) noexcept {
    return impl_->Execute(
        memory,
        moduleBase,
        moduleSize,
        codeBase,
        codeSize,
        subject,
        executionContext,
        request);
}

CoordinateExecutionRuntimeProbe CoordinateExecutionRuntime::Probe()
    const noexcept {
    return impl_->Probe();
}

void CoordinateExecutionRuntime::Reset() noexcept {
    impl_->Reset();
}

}  // namespace lengjing::game::native
