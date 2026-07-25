#include "game/native/CoordinateExecutionRuntime.h"

#include "game/native/Arm64Pacga.h"
#include "game/native/MemoryTransport.h"

#include <unicorn/unicorn.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lengjing::game::native {
namespace {

constexpr std::uint64_t kPageSize = 4096;
constexpr std::uint64_t kPageMask = ~(kPageSize - 1);
constexpr std::uint64_t kSyntheticStackBase = UINT64_C(0x1FFFC0000);
constexpr std::uint64_t kSyntheticStackTop = UINT64_C(0x200000000);
constexpr std::uint64_t kSyntheticStackSize =
    kSyntheticStackTop - kSyntheticStackBase;
constexpr std::size_t kMaximumMappedPages = 4096;

constexpr std::uint32_t kPacgaMask = 0xFFE0FC00U;
constexpr std::uint32_t kPacgaOpcode = 0x9AC03000U;
constexpr std::uint32_t kSvcMask = 0xFFE0001FU;
constexpr std::uint32_t kSvcOpcode = 0xD4000001U;

constexpr std::uint64_t kSysFcntl = 25;
constexpr std::uint64_t kSysIoctl = 29;
constexpr std::uint64_t kSysLseek = 62;
constexpr std::uint64_t kSysFutex = 98;
constexpr std::uint64_t kSysGetPid = 172;
constexpr std::uint64_t kSysGetTid = 178;
constexpr std::uint64_t kSysGetRandom = 278;

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

bool IsSupportedSvc(std::uint64_t number) noexcept {
    return number == kSysFcntl || number == kSysIoctl ||
        number == kSysLseek || number == kSysFutex ||
        number == kSysGetPid || number == kSysGetTid ||
        number == kSysGetRandom;
}

}  // namespace

struct CoordinateExecutionRuntime::Impl {
    struct CachedPage {
        std::uint64_t guestAddress = 0;
        std::uint64_t remoteAddress = 0;
        std::vector<uc_hook> instructionHooks;
    };

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
        CloseEngine();
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

            memory = &memoryValue;
            codeBase = codeBaseValue;
            codeSize = codeSizeValue;
            subject = NormalizeCoordinateExecutionPointer(subjectValue);
            executionContext = executionContextValue;
            request = requestValue;
            plan = executionPlan;
            ctrEl0 = ReadHostCtrEl0();
            counterFrequency = ReadHostCounterFrequency();

            if (plan.verifyReturnStubMagic && !VerifyHostReturnStubMagic()) {
                return Fail(
                    CoordinateExecutionStatus::EvidenceFailure,
                    CoordinateExecutionRuntimeError::ReturnStubInvalid);
            }
            if (!OpenEngine()) {
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
            CloseEngine();
            return result;
        } catch (...) {
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
        self->CaptureWrite(address, size, value);
    }

    static void CodeHook(uc_engine*,
                         std::uint64_t address,
                         std::uint32_t,
                         void* userData) {
        auto* self = static_cast<Impl*>(userData);
        if (self == nullptr || self->hookFailed) return;
        self->HandleInstruction(address);
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
        CloseEngine();
        return Failure(status);
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
            magic == kCoordinateExecutionReturnStubMagic;
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
        return magic == kCoordinateExecutionReturnStubMagic;
    }

    bool OpenEngine() {
        if (uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &engine) != UC_ERR_OK) {
            engine = nullptr;
            probe.error = CoordinateExecutionRuntimeError::EngineSetupFailed;
            return false;
        }
        static_cast<void>(uc_ctl_set_cpu_model(engine, UC_CPU_ARM64_MAX));
        if (uc_mem_map(
                engine,
                kSyntheticStackBase,
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
        for (std::uint64_t page = kSyntheticStackBase;
             page < kSyntheticStackTop;
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
        mappedPages.clear();
        hookedAddresses.clear();
        memoryFaultHook = 0;
        memoryWriteHook = 0;
        mrsHook = 0;
        memory = nullptr;
        hookFailed = false;
        hookRuntimeError = CoordinateExecutionRuntimeError::None;
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
                stackBase, kCoordinateExecutionResultSlotOffset, slot) ||
            !EnsureGuestRange(slot, sizeof(subject))) {
            return false;
        }
        return uc_mem_write(engine, slot, &subject, sizeof(subject)) ==
            UC_ERR_OK;
    }

    bool EnsureGuestRange(std::uint64_t address, std::size_t size) {
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

    bool MapRemotePage(std::uint64_t faultAddress) {
        if (engine == nullptr || memory == nullptr) return false;
        const std::uint64_t guestPage = faultAddress & kPageMask;
        if (mappedPages.find(guestPage) != mappedPages.end()) return true;
        if (pages.size() >= kMaximumMappedPages) {
            probe.error = CoordinateExecutionRuntimeError::GuestPageMapFailed;
            return false;
        }
        const std::uint64_t remotePage =
            NormalizeCoordinateExecutionPointer(guestPage) & kPageMask;
        if (!IsCoordinateExecutionPointer(remotePage)) {
            probe.error = CoordinateExecutionRuntimeError::RemotePageReadFailed;
            return false;
        }

        std::array<std::uint8_t, kPageSize> bytes{};
        game::CoordinateReadDiagnostic diagnostic{};
        if (!ReadRemoteMemory(
                remotePage, bytes.data(), bytes.size(), diagnostic)) {
            probe.read = diagnostic;
            probe.error = CoordinateExecutionRuntimeError::RemotePageReadFailed;
            return false;
        }
        if (uc_mem_map(engine, guestPage, kPageSize, UC_PROT_ALL) != UC_ERR_OK) {
            probe.error = CoordinateExecutionRuntimeError::GuestPageMapFailed;
            return false;
        }
        if (uc_mem_write(
                engine, guestPage, bytes.data(), bytes.size()) != UC_ERR_OK) {
            static_cast<void>(uc_mem_unmap(engine, guestPage, kPageSize));
            probe.error = CoordinateExecutionRuntimeError::GuestPageWriteFailed;
            return false;
        }

        CachedPage page{guestPage, remotePage, {}};
        if (!InstallInstructionHooks(page, bytes)) {
            static_cast<void>(uc_mem_unmap(engine, guestPage, kPageSize));
            return false;
        }
        pages.emplace(guestPage, std::move(page));
        mappedPages.insert(guestPage);
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
        addresses.reserve(16);
        const auto appendIfInPage = [&](std::uint64_t address) {
            if (address >= page.guestAddress &&
                address - page.guestAddress < kPageSize) {
                addresses.push_back(address);
            }
        };
        appendIfInPage(plan.hookPc);
        if (plan.returnStub != 0) appendIfInPage(plan.returnStub);
        for (std::size_t offset = 0; offset <= bytes.size() - 4; offset += 4) {
            std::uint32_t instruction = 0;
            std::memcpy(&instruction, bytes.data() + offset, sizeof(instruction));
            if ((instruction & kPacgaMask) == kPacgaOpcode ||
                (instruction & kSvcMask) == kSvcOpcode) {
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
                probe.error =
                    CoordinateExecutionRuntimeError::InstructionHookSetupFailed;
                return false;
            }
            page.instructionHooks.push_back(hook);
        }
        return true;
    }

    void HandleInstruction(std::uint64_t address) noexcept {
        if (address == plan.hookPc) {
            evidence.hitHookPc = true;
            std::uint64_t x1 = 0;
            if (uc_reg_read(engine, UC_ARM64_REG_X1, &x1) != UC_ERR_OK) {
                FailHook(
                    address,
                    CoordinateExecutionRuntimeError::RegisterSetupFailed);
                return;
            }
            evidence.stackBase = NormalizeCoordinateExecutionPointer(x1);
            if (plan.seedSlotAtHook &&
                !SeedResultSlot(evidence.stackBase)) {
                FailHook(
                    address,
                    CoordinateExecutionRuntimeError::GuestPageWriteFailed);
                return;
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
        if ((instruction & kPacgaMask) == kPacgaOpcode) {
            HandlePacga(address, instruction);
        } else if ((instruction & kSvcMask) == kSvcOpcode) {
            HandleSvc(address);
        }
    }

    void HandlePacga(std::uint64_t address,
                     std::uint32_t instruction) noexcept {
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
        if (executionContext.pacgaOracle.Matches(
                sourceValue, modifierValue)) {
            result = executionContext.pacgaOracle.result;
        } else if (executionContext.HasPacgaKey()) {
            result = ComputeArm64Pacga(
                sourceValue,
                modifierValue,
                Arm64PacgaKey{
                    executionContext.pacgaLow,
                    executionContext.pacgaHigh,
                });
        } else {
            FailHook(address, CoordinateExecutionRuntimeError::PacgaUnavailable);
            return;
        }
        if (!WriteXRegister(destination, result) || !SkipInstruction(address)) {
            FailHook(address, CoordinateExecutionRuntimeError::EmulationFailed);
        }
    }

    void HandleSvc(std::uint64_t address) noexcept {
        std::uint64_t number = 0;
        if (!ReadXRegister(8, number) || !IsSupportedSvc(number)) {
            FailHook(address, CoordinateExecutionRuntimeError::UnsupportedSvc);
            return;
        }

        std::uint64_t result = 0;
        bool success = true;
        if (number == kSysFcntl) {
            std::uint64_t command = 0;
            success = ReadXRegister(1, command);
            result = command == 1 ? 1 : 0;
        } else if (number == kSysFutex) {
            std::uint64_t addressArgument = 0;
            std::uint64_t operation = 0;
            success = ReadXRegister(0, addressArgument) &&
                ReadXRegister(1, operation);
            const std::uint64_t kind = operation & 0x7FU;
            if (success && (kind == 0 || kind == 9)) {
                const std::uint32_t zero = 0;
                success = WriteGuestMemory(
                    addressArgument, &zero, sizeof(zero));
                result = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(-11));
            }
        } else if (number == kSysGetPid || number == kSysGetTid) {
            result = 1000;
        } else if (number == kSysGetRandom) {
            std::uint64_t destination = 0;
            std::uint64_t requested = 0;
            success = ReadXRegister(0, destination) &&
                ReadXRegister(1, requested);
            const std::size_t count = static_cast<std::size_t>(
                std::min<std::uint64_t>(requested, 16));
            std::array<std::uint8_t, 16> bytes{};
            bytes.fill(66);
            if (success && count != 0) {
                success = WriteGuestMemory(destination, bytes.data(), count);
            }
            result = count;
        }

        if (!success || !WriteXRegister(0, result) ||
            !SkipInstruction(address)) {
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
                stackBase, kCoordinateExecutionResultSlotOffset, slot)) {
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
        if (uc_reg_read(engine, UC_ARM64_REG_PC, &pc) != UC_ERR_OK) return;
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
                stackBase, kCoordinateExecutionResultSlotOffset, slot)) {
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
                kCoordinateExecutionPositionOffset,
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

    bool WriteGuestMemory(std::uint64_t address,
                          const void* bytes,
                          std::size_t size) {
        return bytes != nullptr && EnsureGuestRange(address, size) &&
            uc_mem_write(engine, address, bytes, size) == UC_ERR_OK;
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
    std::uint64_t counterFrequency = 0;
    bool hookFailed = false;
    CoordinateExecutionRuntimeError hookRuntimeError =
        CoordinateExecutionRuntimeError::None;
    std::unordered_map<std::uint64_t, CachedPage> pages;
    std::unordered_set<std::uint64_t> mappedPages;
    std::unordered_set<std::uint64_t> hookedAddresses;
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
