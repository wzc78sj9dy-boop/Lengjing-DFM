#include "game/native/CoordinatePoolRuntime.h"

#include "game/native/AlgorithmPositionRuntime.h"
#include "game/native/CoordinatePoolPolicy.h"
#include "game/native/MemoryTransport.h"
#include "game/native/coordinate_pool_internal/FindDec.h"

#include <unicorn/unicorn.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lengjing::game::native {
namespace {

namespace pool = coordinate_pool_internal;

constexpr std::uint64_t kPageSize = 4096;
constexpr std::uint64_t kPageMask = ~(kPageSize - 1);
constexpr std::uint64_t kMinimumRemoteAddress = UINT64_C(0x10000000);
constexpr std::uint64_t kMaximumRemoteAddress = UINT64_C(0x10000000000);
constexpr std::uint64_t kStackTop = UINT64_C(0x7FF00000);
constexpr std::uint64_t kStackSize = UINT64_C(0x10000);
constexpr std::uint64_t kStackBase = kStackTop - kStackSize;
constexpr std::uint64_t kArgumentPage = UINT64_C(0x7FFFF000);
constexpr std::uint64_t kArgumentValueOffset = UINT64_C(0x200);
constexpr std::uint64_t kMaximumCodeSize = UINT64_C(0x200000);
constexpr std::size_t kMaximumCachedPages = 4096;
constexpr std::size_t kV87InstructionBudget = 2000000;
constexpr std::size_t kParameterInstructionBudget = 10000000;
constexpr std::size_t kSearchInstructionBudget = 100000;
constexpr auto kV87Timeout = std::chrono::seconds(2);
constexpr auto kParameterTimeout = std::chrono::seconds(10);
constexpr auto kSearchTimeout = std::chrono::milliseconds(800);
constexpr std::uint32_t kPacgaMask = 0xFFE0FC00U;
constexpr std::uint32_t kPacgaOpcode = 0x9AC03000U;
constexpr std::uint32_t kSvcMask = 0xFFE0001FU;
constexpr std::uint32_t kSvcOpcode = 0xD4000001U;

std::uint64_t NormalizePointer(std::uint64_t value) noexcept {
    return NormalizeCoordinatePoolPointer(value);
}

bool IsRemoteAddress(std::uint64_t value) noexcept {
    return value >= kMinimumRemoteAddress && value < kMaximumRemoteAddress;
}

bool IsValidGuestAddress(std::uint64_t value) noexcept {
    return IsRemoteAddress(NormalizePointer(value));
}

bool AddSignedOffset(std::uint64_t base,
                     std::int32_t offset,
                     std::uint64_t& result) noexcept {
    if (offset >= 0) {
        const auto increment = static_cast<std::uint64_t>(offset);
        if (base > std::numeric_limits<std::uint64_t>::max() - increment) {
            return false;
        }
        result = base + increment;
        return true;
    }
    const auto decrement = static_cast<std::uint64_t>(
        -static_cast<std::int64_t>(offset));
    if (base < decrement) return false;
    result = base - decrement;
    return true;
}

bool IsFinitePosition(const CoordinatePoolPosition& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
        std::isfinite(value.z);
}

int XRegisterId(std::uint32_t index) noexcept {
    if (index <= 28) {
        return UC_ARM64_REG_X0 + static_cast<int>(index);
    }
    if (index == 29) return UC_ARM64_REG_X29;
    if (index == 30) return UC_ARM64_REG_X30;
    return UC_ARM64_REG_XZR;
}

int CapstoneXRegisterId(arm64_reg value) noexcept {
    if (value >= ARM64_REG_W0 && value <= ARM64_REG_W30) {
        value = static_cast<arm64_reg>(
            value - ARM64_REG_W0 + ARM64_REG_X0);
    }
    if (value >= ARM64_REG_X0 && value <= ARM64_REG_X28) {
        return UC_ARM64_REG_X0 + static_cast<int>(value - ARM64_REG_X0);
    }
    if (value == ARM64_REG_X29) return UC_ARM64_REG_X29;
    if (value == ARM64_REG_X30) return UC_ARM64_REG_X30;
    if (value == ARM64_REG_XZR) return UC_ARM64_REG_XZR;
    return UC_ARM64_REG_INVALID;
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

bool FindExecutableMapping(pid_t processId,
                           std::uint64_t address,
                           std::uint64_t& start,
                           std::uint64_t& end) {
    start = 0;
    end = 0;
    if (processId <= 0 || !IsRemoteAddress(address)) return false;

    std::ifstream maps(
        "/proc/" + std::to_string(processId) + "/maps",
        std::ios::binary);
    std::string line;
    while (std::getline(maps, line)) {
        unsigned long long candidateStart = 0;
        unsigned long long candidateEnd = 0;
        char permissions[5]{};
        if (std::sscanf(
                line.c_str(),
                "%llx-%llx %4s",
                &candidateStart,
                &candidateEnd,
                permissions) != 3) {
            continue;
        }
        if (address < candidateStart || address >= candidateEnd ||
            permissions[2] != 'x') {
            continue;
        }
        const std::uint64_t size = candidateEnd - candidateStart;
        if (size == 0 || size > kMaximumCodeSize ||
            (candidateStart & (kPageSize - 1)) != 0 ||
            (size & (kPageSize - 1)) != 0) {
            return false;
        }
        start = candidateStart;
        end = candidateEnd;
        return true;
    }
    return false;
}

}  // namespace

struct CoordinatePoolRuntime::Impl {
    struct CachedPage {
        std::uint64_t guestAddress = 0;
        std::uint64_t remoteAddress = 0;
    };

    struct RingSlot {
        std::uint64_t ring = 0;
        std::uint64_t stamp = 0;
    };

    explicit Impl(CoordinatePoolRuntimeLayout configuredLayout)
        : layout(configuredLayout) {}

    ~Impl() {
        CloseEngineUnlocked();
    }

    bool Configure(const CoordinatePoolRuntimeLayout& configuredLayout) {
        if (!configuredLayout.IsValid()) return false;
        std::lock_guard<std::mutex> lock(mutex);
        ResetUnlocked();
        layout = configuredLayout;
        return true;
    }

    bool Refresh(MemoryTransport& targetMemory,
                 pid_t targetProcessId,
                 std::uintptr_t targetModuleBase,
                 const ProcessExecutionContext& targetExecutionContext,
                 std::uint64_t targetFrame) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!layout.IsValid() || targetProcessId <= 0 ||
            !IsRemoteAddress(targetModuleBase)) {
            SetError(CoordinatePoolRuntimeError::InvalidInput);
            return false;
        }

        const bool bindingChanged = memory != &targetMemory ||
            processId != targetProcessId || moduleBase != targetModuleBase;
        if (bindingChanged) {
            const std::uint64_t attempts = probe.attempts;
            const std::uint64_t successes = probe.successes;
            ResetUnlocked();
            probe.attempts = attempts;
            probe.successes = successes;
            memory = &targetMemory;
            processId = targetProcessId;
            moduleBase = targetModuleBase;
        }

        const bool shouldResolveRoot = finder == nullptr ||
            targetFrame < lastRootFrame ||
            targetFrame - lastRootFrame >= layout.ringRefreshFrames;
        if (shouldResolveRoot) {
            std::uint64_t nextBridge = 0;
            std::uint64_t nextContext = 0;
            std::uint64_t nextEntry = 0;
            if (!ResolveRootUnlocked(
                    nextBridge, nextContext, nextEntry)) {
                SetError(CoordinatePoolRuntimeError::RootReadFailed);
                return false;
            }
            lastRootFrame = targetFrame;
            const bool rootChanged = bridge != nextBridge ||
                decryptContext != nextContext || guestEntry != nextEntry;
            if (rootChanged && finder != nullptr) {
                ResetAnalysisUnlocked();
            }
            bridge = nextBridge;
            decryptContext = nextContext;
            guestEntry = nextEntry;
            probe.bridge = bridge;
            probe.context = decryptContext;
            probe.guestEntry = guestEntry;
            probe.stage = CoordinatePoolRuntimeStage::RootResolved;
        }

        if (finder == nullptr && !AnalyzeCodeUnlocked()) return false;

        probe.threadId = targetExecutionContext.threadId;
        probe.contextGeneration = targetExecutionContext.generation;
        if (!targetExecutionContext.IsUsable()) {
            executionContext = {};
            parametersReady = false;
            ringSlots.clear();
            probe.stage = CoordinatePoolRuntimeStage::CodeAnalyzed;
            SetError(CoordinatePoolRuntimeError::ContextMissing, false);
            return false;
        }

        const bool contextChanged =
            executionContext.threadId != targetExecutionContext.threadId ||
            executionContext.threadStartTimeTicks !=
                targetExecutionContext.threadStartTimeTicks ||
            executionContext.generation != targetExecutionContext.generation ||
            executionContext.tpidrEl0 != targetExecutionContext.tpidrEl0 ||
            executionContext.pacgaLow != targetExecutionContext.pacgaLow ||
            executionContext.pacgaHigh != targetExecutionContext.pacgaHigh ||
            executionContext.pacgaOracle.available !=
                targetExecutionContext.pacgaOracle.available ||
            executionContext.pacgaOracle.data !=
                targetExecutionContext.pacgaOracle.data ||
            executionContext.pacgaOracle.modifier !=
                targetExecutionContext.pacgaOracle.modifier ||
            executionContext.pacgaOracle.result !=
                targetExecutionContext.pacgaOracle.result;
        if (contextChanged) {
            executionContext = targetExecutionContext;
            parametersReady = false;
            computedContext = 0;
            lastPoolPointer = 0;
            ringSlots.clear();
            if (!ClearDynamicPagesUnlocked()) {
                SetError(CoordinatePoolRuntimeError::EngineSetupFailed);
                return false;
            }
        }

        if (!EnsureEngineUnlocked()) {
            SetError(CoordinatePoolRuntimeError::EngineSetupFailed);
            return false;
        }

        if (frame != targetFrame) {
            frame = targetFrame;
            if (!ClearDynamicPagesUnlocked()) {
                SetError(CoordinatePoolRuntimeError::EngineSetupFailed);
                return false;
            }
        }
        probe.error = CoordinatePoolRuntimeError::None;
        probe.stage = CoordinatePoolRuntimeStage::ContextReady;
        return true;
    }

    bool ReadPosition(std::uintptr_t component,
                      CoordinatePoolPosition& position) {
        std::lock_guard<std::mutex> lock(mutex);
        position = {};
        ++probe.attempts;
        if (memory == nullptr || finder == nullptr || engine == nullptr ||
            !executionContext.IsUsable() || !IsValidGuestAddress(component)) {
            SetError(CoordinatePoolRuntimeError::InvalidInput);
            return false;
        }
        const bool refreshParameters = !parametersReady ||
            ShouldRefreshCoordinatePoolState(
                frame, lastParameterFrame, layout.ringRefreshFrames);
        if (refreshParameters && !PrepareParametersUnlocked(component)) {
            return false;
        }
        if (refreshParameters) {
            lastParameterFrame = frame;
            ringSlots.clear();
        }
        RefreshPoolPointerUnlocked();
        if (!parametersReady) {
            if (!PrepareParametersUnlocked(component)) return false;
            lastParameterFrame = frame;
            ringSlots.clear();
            RefreshPoolPointerUnlocked();
        }

        RingSlot* selected = nullptr;
        auto found = ringSlots.find(component);
        const bool stale = found != ringSlots.end() &&
            frame >= found->second.stamp &&
            frame - found->second.stamp >= layout.ringRefreshFrames;
        bool usedCachedRing = false;
        if (found == ringSlots.end() || found->second.ring == 0 || stale) {
            std::uint64_t ring = 0;
            if (ExecuteRingSearchUnlocked(component, ring)) {
                RingSlot& slot = ringSlots[component];
                slot.ring = ring;
                slot.stamp = frame;
                selected = &slot;
            } else if (found != ringSlots.end() && found->second.ring != 0) {
                selected = &found->second;
                usedCachedRing = true;
            } else {
                SetError(CoordinatePoolRuntimeError::RingSearchFailed);
                return false;
            }
        } else {
            selected = &found->second;
            usedCachedRing = true;
        }

        CoordinatePoolPosition candidate{};
        bool stable = ReadStablePositionUnlocked(selected->ring, candidate);
        if (ShouldRetryCoordinatePoolRing(usedCachedRing, stable)) {
            ringSlots.erase(component);
            if (ClearDynamicPagesUnlocked()) {
                std::uint64_t refreshedRing = 0;
                if (ExecuteRingSearchUnlocked(component, refreshedRing)) {
                    RingSlot& slot = ringSlots[component];
                    slot = RingSlot{refreshedRing, frame};
                    stable = ReadStablePositionUnlocked(
                        refreshedRing, candidate);
                }
            }
        }
        if (!stable || !IsFinitePosition(candidate)) {
            SetError(CoordinatePoolRuntimeError::PositionReadFailed);
            return false;
        }

        position = candidate;
        ++probe.successes;
        probe.error = CoordinatePoolRuntimeError::None;
        probe.stage = CoordinatePoolRuntimeStage::Active;
        return true;
    }

    CoordinatePoolRuntimeProbe Probe() const {
        std::lock_guard<std::mutex> lock(mutex);
        return probe;
    }

    void Reset() {
        std::lock_guard<std::mutex> lock(mutex);
        ResetUnlocked();
    }

private:
    static bool MemoryFaultHook(uc_engine*,
                                uc_mem_type,
                                std::uint64_t address,
                                int,
                                std::int64_t,
                                void* userData) {
        auto* self = static_cast<Impl*>(userData);
        if (self == nullptr) return false;
        const bool loaded = self->LoadRemotePageUnlocked(address);
        if (!loaded) self->hookFailed = true;
        return loaded;
    }

    static void CodeHook(uc_engine*,
                         std::uint64_t address,
                         std::uint32_t,
                         void* userData) {
        auto* self = static_cast<Impl*>(userData);
        if (self == nullptr || self->hookFailed) return;
        self->HandleInstructionUnlocked(address);
    }

    static std::uint32_t MrsHook(uc_engine* targetEngine,
                                 uc_arm64_reg destination,
                                 const uc_arm64_cp_reg* systemRegister,
                                 void* userData) {
        auto* self = static_cast<Impl*>(userData);
        if (self == nullptr || systemRegister == nullptr) return 0;
        std::uint64_t value = 0;
        if (systemRegister->op0 != 3 || systemRegister->op1 != 3) {
            return 0;
        }
        if (systemRegister->crn == 0 && systemRegister->crm == 0 &&
            systemRegister->op2 == 1) {
            value = ReadHostCtrEl0();
        } else if (systemRegister->crn == 13 &&
                   systemRegister->crm == 0 &&
                   systemRegister->op2 == 2) {
            value = self->executionContext.tpidrEl0;
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
        if (uc_reg_write(targetEngine, destination, &value) != UC_ERR_OK) {
            self->FailHookUnlocked();
        }
        return 1;
    }

    bool ResolveRootUnlocked(std::uint64_t& nextBridge,
                             std::uint64_t& nextContext,
                             std::uint64_t& nextEntry) {
        nextBridge = 0;
        nextContext = 0;
        nextEntry = 0;
        if (memory == nullptr ||
            layout.rootRva >
                std::numeric_limits<std::uintptr_t>::max() - moduleBase ||
            layout.bridgeOffset >
                std::numeric_limits<std::uintptr_t>::max() -
                    moduleBase - layout.rootRva) {
            return false;
        }
        std::uint64_t rawBridge = 0;
        if (!ReadRemoteUnlocked(
                moduleBase + layout.rootRva + layout.bridgeOffset,
                &rawBridge,
                sizeof(rawBridge))) {
            return false;
        }
        nextBridge = NormalizePointer(rawBridge);
        std::uint64_t rawContext = 0;
        std::uint64_t rawEntry = 0;
        std::uint64_t contextAddress = 0;
        if (!IsRemoteAddress(nextBridge) ||
            !AddSignedOffset(
                nextBridge, layout.contextOffset, contextAddress) ||
            layout.entryOffset >
                std::numeric_limits<std::uint64_t>::max() - nextBridge ||
            !IsRemoteAddress(contextAddress) ||
            !ReadRemoteUnlocked(
                contextAddress, &rawContext, sizeof(rawContext)) ||
            !ReadRemoteUnlocked(
                nextBridge + layout.entryOffset,
                &rawEntry,
                sizeof(rawEntry))) {
            return false;
        }
        nextContext = rawContext;
        nextEntry = NormalizePointer(rawEntry);
        return IsValidGuestAddress(nextContext) &&
            IsRemoteAddress(nextEntry) && (nextEntry & 3U) == 0;
    }

    bool AnalyzeCodeUnlocked() {
        std::uint64_t mappingStart = 0;
        std::uint64_t mappingEnd = 0;
        if (!FindExecutableMapping(
                processId, guestEntry, mappingStart, mappingEnd)) {
            SetError(CoordinatePoolRuntimeError::EntryMappingMissing);
            return false;
        }
        const std::size_t mappingSize = static_cast<std::size_t>(
            mappingEnd - mappingStart);
        std::vector<std::uint8_t> bytes(mappingSize);
        for (std::size_t offset = 0; offset < bytes.size();
             offset += kPageSize) {
            const std::size_t count = std::min<std::size_t>(
                kPageSize, bytes.size() - offset);
            if (!ReadRemoteUnlocked(
                    mappingStart + offset, bytes.data() + offset, count)) {
                SetError(CoordinatePoolRuntimeError::CodeReadFailed);
                return false;
            }
        }

        auto candidate = std::unique_ptr<pool::coord_dec::FindDec>(
            new (std::nothrow) pool::coord_dec::FindDec());
        if (candidate == nullptr ||
            candidate->set(
                mappingStart,
                bytes.data(),
                static_cast<std::uint32_t>(bytes.size())) != 0 ||
            candidate->find_dec(guestEntry) != 0) {
            SetError(CoordinatePoolRuntimeError::AnalysisFailed);
            return false;
        }

        pool::method* entry =
            candidate->get_shellcode()->get_method("entry");
        pool::point* v87 = entry != nullptr
            ? entry->get_point("v87_end")
            : nullptr;
        pool::point* search = entry != nullptr
            ? entry->get_point("hash_end")
            : nullptr;
        pool::point* parameters = entry != nullptr
            ? entry->get_point("all_params_exec_end")
            : nullptr;
        if (entry == nullptr || v87 == nullptr || search == nullptr ||
            parameters == nullptr || candidate->index_expr == nullptr ||
            candidate->pool_ptr_offset == 0 ||
            v87->reg == ARM64_REG_INVALID ||
            search->reg == ARM64_REG_INVALID) {
            SetError(CoordinatePoolRuntimeError::AnalysisFailed);
            return false;
        }

        finder = std::move(candidate);
        codeBase = mappingStart;
        codeSize = mappingSize;
        entryStart = entry->start_address();
        v87End = v87->address;
        v87Register = v87->reg;
        searchEnd = search->address;
        searchRegister = search->reg;
        parameterEnd = parameters->address;
        probe.codeBase = codeBase;
        probe.codeSize = codeSize;
        probe.poolPointerOffset = finder->pool_ptr_offset;
        probe.indexOffset = finder->index_offset;
        probe.ringOffset = finder->get_ring_offset();
        probe.error = CoordinatePoolRuntimeError::None;
        probe.stage = CoordinatePoolRuntimeStage::CodeAnalyzed;
        return true;
    }

    bool EnsureEngineUnlocked() {
        if (engine != nullptr) return true;
        if (finder == nullptr || codeBase == 0 || codeSize == 0 ||
            uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &engine) != UC_ERR_OK) {
            engine = nullptr;
            return false;
        }
        static_cast<void>(uc_ctl_set_cpu_model(engine, UC_CPU_ARM64_MAX));
        if (uc_mem_map(engine, codeBase, codeSize, UC_PROT_ALL) != UC_ERR_OK ||
            uc_mem_write(
                engine,
                codeBase,
                finder->get_shellcode()->data(),
                codeSize) != UC_ERR_OK ||
            uc_mem_map(
                engine, kStackBase, kStackSize, UC_PROT_READ | UC_PROT_WRITE) !=
                UC_ERR_OK ||
            uc_mem_map(
                engine,
                kArgumentPage,
                kPageSize,
                UC_PROT_READ | UC_PROT_WRITE) != UC_ERR_OK ||
            uc_hook_add(
                engine,
                &memoryHook,
                UC_HOOK_MEM_READ_UNMAPPED |
                    UC_HOOK_MEM_WRITE_UNMAPPED |
                    UC_HOOK_MEM_FETCH_UNMAPPED,
                reinterpret_cast<void*>(MemoryFaultHook),
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
            CloseEngineUnlocked();
            return false;
        }
        return true;
    }

    bool PrepareRegistersUnlocked(std::uint64_t x0,
                                  std::uint64_t argumentValue,
                                  bool useArgumentPage) {
        if (engine == nullptr) return false;
        static const std::array<std::uint8_t, kStackSize> zeroStack{};
        static const std::array<std::uint8_t, kPageSize> zeroPage{};
        if (uc_mem_write(
                engine, kStackBase, zeroStack.data(), zeroStack.size()) !=
                UC_ERR_OK ||
            uc_mem_write(
                engine, kArgumentPage, zeroPage.data(), zeroPage.size()) !=
                UC_ERR_OK) {
            return false;
        }

        const std::uint64_t zero = 0;
        for (std::uint32_t index = 0; index <= 30; ++index) {
            const std::uint64_t value = index == 0 ? x0 : zero;
            if (uc_reg_write(engine, XRegisterId(index), &value) != UC_ERR_OK) {
                return false;
            }
        }
        const std::uint64_t x1 = useArgumentPage ? kArgumentPage : 0;
        if (useArgumentPage &&
            uc_mem_write(
                engine,
                kArgumentPage + kArgumentValueOffset,
                &argumentValue,
                sizeof(argumentValue)) != UC_ERR_OK) {
            return false;
        }
        if (uc_reg_write(engine, UC_ARM64_REG_X1, &x1) != UC_ERR_OK ||
            uc_reg_write(engine, UC_ARM64_REG_SP, &kStackTop) != UC_ERR_OK ||
            uc_reg_write(
                engine,
                UC_ARM64_REG_TPIDR_EL0,
                &executionContext.tpidrEl0) != UC_ERR_OK ||
            uc_reg_write(engine, UC_ARM64_REG_NZCV, &zero) != UC_ERR_OK ||
            uc_reg_write(engine, UC_ARM64_REG_FPCR, &zero) != UC_ERR_OK ||
            uc_reg_write(engine, UC_ARM64_REG_FPSR, &zero) != UC_ERR_OK) {
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

    bool RunStageUnlocked(std::uint64_t end,
                          std::size_t instructionBudget,
                          std::chrono::microseconds stageTimeout) {
        if (engine == nullptr || entryStart == 0 || end <= entryStart) {
            return false;
        }
        hookFailed = false;
        const auto timeout = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                stageTimeout).count());
        const uc_err error = uc_emu_start(
            engine, entryStart, end, timeout, instructionBudget);
        std::uint64_t pc = 0;
        const uc_err readError = uc_reg_read(
            engine, UC_ARM64_REG_PC, &pc);
        return error == UC_ERR_OK && !hookFailed &&
            readError == UC_ERR_OK && pc == end;
    }

    bool PrepareParametersUnlocked(std::uint64_t component) {
        if (!EnsureEngineUnlocked() || computedContext == 0) {
            if (!PrepareRegistersUnlocked(decryptContext, 0, false)) {
                SetError(CoordinatePoolRuntimeError::ParameterExecutionFailed);
                return false;
            }
            if (!RunStageUnlocked(
                    v87End, kV87InstructionBudget, kV87Timeout)) {
                SetError(CoordinatePoolRuntimeError::ParameterExecutionFailed);
                return false;
            }
            const int registerId = CapstoneXRegisterId(v87Register);
            if (registerId == UC_ARM64_REG_INVALID ||
                uc_reg_read(engine, registerId, &computedContext) != UC_ERR_OK ||
                computedContext == 0) {
                SetError(CoordinatePoolRuntimeError::ParameterExecutionFailed);
                return false;
            }
        }

        if (!ClearDynamicPagesUnlocked()) {
            SetError(CoordinatePoolRuntimeError::EngineSetupFailed);
            return false;
        }

        if (layout.componentKeyOffset >
            std::numeric_limits<std::uint64_t>::max() - component) {
            SetError(CoordinatePoolRuntimeError::ParameterExecutionFailed);
            return false;
        }
        if (!PrepareRegistersUnlocked(
                decryptContext,
                component + layout.componentKeyOffset,
                true)) {
            SetError(CoordinatePoolRuntimeError::ParameterExecutionFailed);
            return false;
        }
        captureParameters = true;
        allowMissingRemotePages = true;
        const bool ran = RunStageUnlocked(
            parameterEnd,
            kParameterInstructionBudget,
            kParameterTimeout);
        allowMissingRemotePages = false;
        captureParameters = false;
        if (!ran) {
            SetError(CoordinatePoolRuntimeError::ParameterExecutionFailed);
            return false;
        }

        std::uint64_t stackPointer = 0;
        if (uc_reg_read(engine, UC_ARM64_REG_SP, &stackPointer) != UC_ERR_OK) {
            SetError(CoordinatePoolRuntimeError::ParameterReadFailed);
            return false;
        }
        for (auto& parameter : finder->mem_param_list) {
            if (parameter.size == 0 ||
                parameter.size > sizeof(parameter.value)) {
                SetError(CoordinatePoolRuntimeError::ParameterReadFailed);
                return false;
            }
            const std::size_t captureSize = parameter.offset.empty()
                ? parameter.size
                : sizeof(parameter.value);
            parameter.value = 0;
            const std::uint64_t stackAddress = stackPointer +
                static_cast<std::int64_t>(parameter.disp);
            if (uc_mem_read(
                    engine,
                    stackAddress,
                    &parameter.value,
                    captureSize) != UC_ERR_OK ||
                !ResolveParameterUnlocked(parameter)) {
                SetError(CoordinatePoolRuntimeError::ParameterReadFailed);
                return false;
            }
        }
        finder->setup_param();
        if (!ClearDynamicPagesUnlocked()) {
            SetError(CoordinatePoolRuntimeError::EngineSetupFailed);
            return false;
        }
        parametersReady = true;
        probe.error = CoordinatePoolRuntimeError::None;
        return true;
    }

    bool ResolveParameterUnlocked(pool::coord_dec::param& parameter) {
        if (parameter.offset.empty()) return true;
        std::uint64_t pointer = NormalizePointer(parameter.value);
        if (!IsRemoteAddress(pointer)) return false;
        for (std::size_t index = 0; index < parameter.offset.size(); ++index) {
            const std::int64_t displacement = parameter.offset[index];
            const std::uint64_t address = NormalizePointer(
                pointer + static_cast<std::uint64_t>(displacement));
            if (!IsRemoteAddress(address)) return false;
            if (index + 1 == parameter.offset.size()) {
                std::uint64_t value = 0;
                if (!ReadRemoteUnlocked(
                        address, &value, parameter.size)) {
                    return false;
                }
                parameter.value = value;
                return true;
            }
            std::uint64_t next = 0;
            if (!ReadRemoteUnlocked(address, &next, sizeof(next))) {
                return false;
            }
            pointer = NormalizePointer(next);
            if (!IsRemoteAddress(pointer)) return false;
        }
        return false;
    }

    bool ReadStablePositionUnlocked(
        std::uint64_t ring,
        CoordinatePoolPosition& candidate) {
        candidate = {};
        if (finder == nullptr) return false;
        const std::uint64_t indexAddress = NormalizePointer(
            ring + static_cast<std::uint64_t>(finder->index_offset));
        if (!IsRemoteAddress(indexAddress)) return false;

        for (int attempt = 0;
             attempt < kCoordinatePoolStableReadAttempts;
             ++attempt) {
            std::uint64_t index = 0;
            std::uint64_t indexAfter = 0;
            if (!ReadRemoteUnlocked(indexAddress, &index, sizeof(index))) {
                continue;
            }
            const std::uint64_t poolSlot = finder->decode_ring_slot(index);
            const std::uint64_t fixedOffset =
                static_cast<std::uint64_t>(finder->get_ring_offset()) +
                layout.poolHeadSkip;
            if (ring > std::numeric_limits<std::uint64_t>::max() -
                           fixedOffset ||
                poolSlot >
                    (std::numeric_limits<std::uint64_t>::max() - ring -
                     fixedOffset) /
                        layout.entryStride) {
                return false;
            }
            const std::uint64_t coordinateAddress = NormalizePointer(
                ring + fixedOffset +
                static_cast<std::uint64_t>(layout.entryStride) * poolSlot);
            CoordinatePoolPosition value{};
            if (!IsRemoteAddress(coordinateAddress) ||
                !ReadRemoteUnlocked(
                    coordinateAddress, &value, sizeof(value)) ||
                !ReadRemoteUnlocked(
                    indexAddress, &indexAfter, sizeof(indexAfter))) {
                continue;
            }
            if (index == indexAfter && IsFinitePosition(value)) {
                candidate = value;
                return true;
            }
        }
        return false;
    }

    void RefreshPoolPointerUnlocked() {
        if (!parametersReady || computedContext == 0 ||
            finder->pool_ptr_offset == 0) {
            return;
        }
        const std::uint64_t address = NormalizePointer(
            computedContext +
            static_cast<std::int64_t>(finder->pool_ptr_offset));
        std::uint64_t pointer = 0;
        if (!IsRemoteAddress(address) ||
            !ReadRemoteUnlocked(address, &pointer, sizeof(pointer))) {
            return;
        }
        pointer = NormalizePointer(pointer);
        if (!IsRemoteAddress(pointer)) return;
        if (lastPoolPointer != 0 && lastPoolPointer != pointer) {
            ringSlots.clear();
            parametersReady = false;
        }
        lastPoolPointer = pointer;
    }

    bool ExecuteRingSearchUnlocked(std::uint64_t component,
                                   std::uint64_t& ring) {
        ring = 0;
        if (layout.componentKeyOffset >
            std::numeric_limits<std::uint64_t>::max() - component ||
            !PrepareRegistersUnlocked(
                decryptContext,
                component + layout.componentKeyOffset,
                true) ||
            !RunStageUnlocked(
                searchEnd,
                kSearchInstructionBudget,
                kSearchTimeout)) {
            return false;
        }
        const int registerId = CapstoneXRegisterId(searchRegister);
        std::uint64_t rawRing = 0;
        if (registerId == UC_ARM64_REG_INVALID ||
            uc_reg_read(engine, registerId, &rawRing) != UC_ERR_OK) {
            return false;
        }
        ring = NormalizePointer(rawRing);
        return IsRemoteAddress(ring);
    }

    bool ReadRemoteUnlocked(std::uint64_t address,
                            void* destination,
                            std::size_t size) {
        const std::uint64_t normalized = NormalizePointer(address);
        return memory != nullptr && destination != nullptr && size != 0 &&
            IsRemoteAddress(normalized) &&
            normalized <= kMaximumRemoteAddress - size &&
            memory->Read(normalized, destination, size);
    }

    bool LoadRemotePageUnlocked(std::uint64_t faultAddress) {
        if (engine == nullptr || memory == nullptr) return false;
        const std::uint64_t guestPage = faultAddress & kPageMask;
        if ((guestPage >= kStackBase && guestPage < kStackTop) ||
            guestPage == kArgumentPage ||
            (guestPage >= codeBase && guestPage < codeBase + codeSize)) {
            return true;
        }
        const auto existing = std::find_if(
            dynamicPages.begin(),
            dynamicPages.end(),
            [guestPage](const CachedPage& page) {
                return page.guestAddress == guestPage;
            });
        if (existing != dynamicPages.end()) return true;
        if (dynamicPages.size() >= kMaximumCachedPages) return false;

        const std::uint64_t remotePage = NormalizePointer(guestPage) & kPageMask;
        std::array<std::uint8_t, kPageSize> data{};
        const bool remoteReadable = IsRemoteAddress(remotePage) &&
            remotePage <= kMaximumRemoteAddress - kPageSize &&
            memory->Read(remotePage, data.data(), data.size());
        if (!remoteReadable && !allowMissingRemotePages) {
            return false;
        }
        if (uc_mem_map(engine, guestPage, kPageSize, UC_PROT_ALL) != UC_ERR_OK) {
            return false;
        }
        if (uc_mem_write(engine, guestPage, data.data(), data.size()) !=
            UC_ERR_OK) {
            static_cast<void>(uc_mem_unmap(engine, guestPage, kPageSize));
            return false;
        }
        dynamicPages.push_back({guestPage, remotePage});
        return true;
    }

    bool ClearDynamicPagesUnlocked() {
        if (engine == nullptr) {
            dynamicPages.clear();
            return true;
        }
        for (const CachedPage& page : dynamicPages) {
            if (uc_mem_unmap(engine, page.guestAddress, kPageSize) !=
                UC_ERR_OK) {
                return false;
            }
        }
        dynamicPages.clear();
        return true;
    }

    void HandleInstructionUnlocked(std::uint64_t address) {
        if (captureParameters) {
            for (auto& parameter : finder->analyze.varParams) {
                if (parameter.addr != address) continue;
                const int registerId = CapstoneXRegisterId(parameter.reg);
                if (registerId == UC_ARM64_REG_INVALID ||
                    uc_reg_read(engine, registerId, &parameter.value) !=
                        UC_ERR_OK) {
                    FailHookUnlocked();
                    return;
                }
            }
        }

        std::uint32_t instruction = 0;
        if (uc_mem_read(
                engine, address, &instruction, sizeof(instruction)) !=
                UC_ERR_OK) {
            FailHookUnlocked();
            return;
        }
        if ((instruction & kPacgaMask) == kPacgaOpcode) {
            const std::uint32_t destination = instruction & 0x1FU;
            const std::uint32_t source = (instruction >> 5U) & 0x1FU;
            const std::uint32_t modifier = (instruction >> 16U) & 0x1FU;
            std::uint64_t sourceValue = 0;
            std::uint64_t modifierValue = 0;
            if (!ReadXRegisterUnlocked(source, sourceValue) ||
                !ReadXRegisterUnlocked(modifier, modifierValue)) {
                FailHookUnlocked();
                return;
            }
            std::uint64_t result = 0;
            if (executionContext.pacgaOracle.Matches(
                    sourceValue, modifierValue)) {
                result = executionContext.pacgaOracle.result;
            } else if (executionContext.HasPacgaKey()) {
                result = ComputeAlgorithmPositionPacga(
                    sourceValue,
                    modifierValue,
                    AlgorithmPacgaKey{
                        executionContext.pacgaLow,
                        executionContext.pacgaHigh,
                    });
            } else {
                FailHookUnlocked();
                return;
            }
            if (!WriteXRegisterUnlocked(destination, result) ||
                !SkipInstructionUnlocked(address)) {
                FailHookUnlocked();
            }
        } else if ((instruction & kSvcMask) == kSvcOpcode) {
            FailHookUnlocked();
        }
    }

    bool ReadXRegisterUnlocked(std::uint32_t index,
                               std::uint64_t& value) const {
        value = 0;
        return index == 31 ||
            uc_reg_read(engine, XRegisterId(index), &value) == UC_ERR_OK;
    }

    bool WriteXRegisterUnlocked(std::uint32_t index,
                                std::uint64_t value) {
        return index == 31 ||
            uc_reg_write(engine, XRegisterId(index), &value) == UC_ERR_OK;
    }

    bool SkipInstructionUnlocked(std::uint64_t address) {
        if (address > std::numeric_limits<std::uint64_t>::max() - 4) {
            return false;
        }
        const std::uint64_t next = address + 4;
        return uc_reg_write(engine, UC_ARM64_REG_PC, &next) == UC_ERR_OK;
    }

    void FailHookUnlocked() {
        hookFailed = true;
        if (engine != nullptr) static_cast<void>(uc_emu_stop(engine));
    }

    void SetError(CoordinatePoolRuntimeError error,
                  bool faulted = true) {
        probe.error = error;
        if (faulted) probe.stage = CoordinatePoolRuntimeStage::Faulted;
    }

    void ResetAnalysisUnlocked() {
        CloseEngineUnlocked();
        finder.reset();
        bridge = 0;
        decryptContext = 0;
        guestEntry = 0;
        codeBase = 0;
        codeSize = 0;
        entryStart = 0;
        v87End = 0;
        searchEnd = 0;
        parameterEnd = 0;
        v87Register = ARM64_REG_INVALID;
        searchRegister = ARM64_REG_INVALID;
        computedContext = 0;
        parametersReady = false;
        lastParameterFrame = 0;
        lastPoolPointer = 0;
        ringSlots.clear();
        probe.bridge = 0;
        probe.context = 0;
        probe.guestEntry = 0;
        probe.codeBase = 0;
        probe.codeSize = 0;
        probe.poolPointerOffset = 0;
        probe.indexOffset = 0;
        probe.ringOffset = 0;
        probe.stage = CoordinatePoolRuntimeStage::Idle;
        probe.error = CoordinatePoolRuntimeError::None;
    }

    void CloseEngineUnlocked() {
        dynamicPages.clear();
        if (engine != nullptr) {
            static_cast<void>(uc_close(engine));
            engine = nullptr;
        }
        memoryHook = 0;
        codeHook = 0;
        mrsHook = 0;
        hookFailed = false;
        captureParameters = false;
        allowMissingRemotePages = false;
    }

    void ResetUnlocked() {
        const CoordinatePoolRuntimeLayout configuredLayout = layout;
        CloseEngineUnlocked();
        finder.reset();
        memory = nullptr;
        processId = -1;
        moduleBase = 0;
        executionContext = {};
        bridge = 0;
        decryptContext = 0;
        guestEntry = 0;
        codeBase = 0;
        codeSize = 0;
        entryStart = 0;
        v87End = 0;
        searchEnd = 0;
        parameterEnd = 0;
        v87Register = ARM64_REG_INVALID;
        searchRegister = ARM64_REG_INVALID;
        computedContext = 0;
        parametersReady = false;
        frame = 0;
        lastRootFrame = 0;
        lastParameterFrame = 0;
        lastPoolPointer = 0;
        ringSlots.clear();
        probe = {};
        layout = configuredLayout;
    }

    CoordinatePoolRuntimeLayout layout{};
    mutable std::mutex mutex;
    MemoryTransport* memory = nullptr;
    pid_t processId = -1;
    std::uintptr_t moduleBase = 0;
    ProcessExecutionContext executionContext{};
    CoordinatePoolRuntimeProbe probe{};
    std::uint64_t bridge = 0;
    std::uint64_t decryptContext = 0;
    std::uint64_t guestEntry = 0;
    std::uint64_t codeBase = 0;
    std::size_t codeSize = 0;
    std::uint64_t entryStart = 0;
    std::uint64_t v87End = 0;
    arm64_reg v87Register = ARM64_REG_INVALID;
    std::uint64_t searchEnd = 0;
    arm64_reg searchRegister = ARM64_REG_INVALID;
    std::uint64_t parameterEnd = 0;
    std::uint64_t computedContext = 0;
    bool parametersReady = false;
    std::uint64_t frame = 0;
    std::uint64_t lastRootFrame = 0;
    std::uint64_t lastParameterFrame = 0;
    std::uint64_t lastPoolPointer = 0;
    std::unordered_map<std::uint64_t, RingSlot> ringSlots;
    std::unique_ptr<pool::coord_dec::FindDec> finder;
    uc_engine* engine = nullptr;
    uc_hook memoryHook = 0;
    uc_hook codeHook = 0;
    uc_hook mrsHook = 0;
    std::vector<CachedPage> dynamicPages;
    bool hookFailed = false;
    bool captureParameters = false;
    bool allowMissingRemotePages = false;
};

CoordinatePoolRuntime::CoordinatePoolRuntime(
    CoordinatePoolRuntimeLayout layout)
    : impl_(new (std::nothrow) Impl(layout)) {}

CoordinatePoolRuntime::~CoordinatePoolRuntime() = default;

bool CoordinatePoolRuntime::Configure(
    const CoordinatePoolRuntimeLayout& layout) noexcept {
    try {
        return impl_ != nullptr && impl_->Configure(layout);
    } catch (...) {
        return false;
    }
}

bool CoordinatePoolRuntime::Refresh(
    MemoryTransport& memory,
    pid_t processId,
    std::uintptr_t moduleBase,
    const ProcessExecutionContext& executionContext,
    std::uint64_t frame) noexcept {
    try {
        return impl_ != nullptr && impl_->Refresh(
            memory, processId, moduleBase, executionContext, frame);
    } catch (...) {
        return false;
    }
}

bool CoordinatePoolRuntime::ReadPosition(
    std::uintptr_t component,
    CoordinatePoolPosition& position) noexcept {
    position = {};
    try {
        return impl_ != nullptr && impl_->ReadPosition(component, position);
    } catch (...) {
        position = {};
        return false;
    }
}

CoordinatePoolRuntimeProbe CoordinatePoolRuntime::Probe() const noexcept {
    try {
        return impl_ != nullptr ? impl_->Probe() : CoordinatePoolRuntimeProbe{};
    } catch (...) {
        return {};
    }
}

void CoordinatePoolRuntime::Reset() noexcept {
    try {
        if (impl_ != nullptr) impl_->Reset();
    } catch (...) {
    }
}

}  // namespace lengjing::game::native
