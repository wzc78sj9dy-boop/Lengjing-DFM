#include "game/native/CoordinatePoolRuntime.h"

#include "game/native/AlgorithmPositionRuntime.h"
#include "game/native/CoordinatePoolPolicy.h"
#include "game/native/MemoryTransport.h"
#include "game/native/coordinate_pool_internal/FindDec.h"

#include <capstone/capstone.h>
#include <unicorn/unicorn.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
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
constexpr std::size_t kEntryAnalysisInstructionLimit = 5000;
constexpr std::size_t kDecodeAnalysisInstructionLimit = 500;
constexpr std::uint32_t kArm64Nop = UINT32_C(0xD503201F);
constexpr std::size_t kMaximumCachedPages = 4096;
constexpr std::size_t kRootSnapshotReadLimit = 3;
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
constexpr std::uint32_t kPoolPointerFinderReady = 1U << 0U;
constexpr std::uint32_t kPoolPointerParametersReady = 1U << 1U;
constexpr std::uint32_t kPoolPointerContextReady = 1U << 2U;
constexpr std::uint32_t kPoolPointerOffsetReady = 1U << 3U;

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

struct EntryCodeScan {
    std::uint64_t returnAddress = 0;
};

bool ScanEntryCode(const std::vector<std::uint8_t>& bytes,
                   std::uint64_t mappingStart,
                   std::uint64_t scanStart,
                   std::uint64_t scanEnd,
                   EntryCodeScan& scan) {
    scan = {};
    if (scanStart < mappingStart || scanEnd <= scanStart ||
        scanEnd - mappingStart > bytes.size()) {
        return false;
    }

    csh handle = 0;
    if (cs_open(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN, &handle) != CS_ERR_OK) {
        return false;
    }
    static_cast<void>(cs_option(handle, CS_OPT_SKIPDATA, CS_OPT_ON));
    cs_insn* instructions = nullptr;
    const std::size_t offset = static_cast<std::size_t>(
        scanStart - mappingStart);
    const std::size_t count = cs_disasm(
        handle,
        bytes.data() + offset,
        static_cast<std::size_t>(scanEnd - scanStart),
        scanStart,
        0,
        &instructions);
    if (count == 0 || instructions == nullptr) {
        cs_close(&handle);
        return false;
    }

    for (std::size_t index = 0; index < count; ++index) {
        const cs_insn& instruction = instructions[index];
        if (instruction.id == ARM64_INS_RET) {
            scan.returnAddress = instruction.address;
            break;
        }
    }
    cs_free(instructions, count);
    cs_close(&handle);
    return true;
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
                           std::uint64_t& end,
                           int* systemError = nullptr) {
    start = 0;
    end = 0;
    if (systemError != nullptr) *systemError = 0;
    if (processId <= 0 || !IsRemoteAddress(address)) {
        if (systemError != nullptr) *systemError = -EINVAL;
        return false;
    }

    errno = 0;
    std::ifstream maps(
        "/proc/" + std::to_string(processId) + "/maps",
        std::ios::binary);
    if (!maps) {
        if (systemError != nullptr) {
            *systemError = errno != 0 ? -errno : -EIO;
        }
        return false;
    }
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
            if (systemError != nullptr) *systemError = -ERANGE;
            return false;
        }
        start = candidateStart;
        end = candidateEnd;
        return true;
    }
    if (systemError != nullptr) {
        *systemError = maps.bad() ? -EIO : -ENOENT;
    }
    return false;
}

}  // namespace

struct CoordinatePoolRuntime::Impl {
    struct CachedPage {
        std::uint64_t guestAddress = 0;
        std::uint64_t remoteAddress = 0;
        std::vector<uc_hook> instructionHooks;
    };

    struct CodeRangeFingerprint {
        std::uint64_t remoteAddress = 0;
        std::size_t size = 0;
        std::uint64_t fingerprint = 0;
    };

    enum class CodeValidationResult : std::uint8_t {
        Unchanged,
        Changed,
        Unavailable,
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
        ClearReadDiagnosticUnlocked();
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

        CoordinatePoolRootSnapshot nextRoot{};
        int rootStatus = 0;
        if (!ResolveRootUnlocked(nextRoot, rootStatus)) {
            if (rootStatus != 0 && !probe.read.HasFailure()) {
                SetError(
                    CoordinatePoolRuntimeError::RootReadFailed,
                    true,
                    rootStatus);
            } else {
                SetError(CoordinatePoolRuntimeError::RootReadFailed);
            }
            return false;
        }

        const CoordinatePoolRootSnapshot previousRoot{
            bridge,
            decryptContext,
            guestEntry,
        };
        const bool codeIdentityChanged = finder != nullptr &&
            CoordinatePoolCodeIdentityChanged(previousRoot, nextRoot);
        const bool runtimeContextChanged = finder != nullptr &&
            CoordinatePoolContextIdentityChanged(previousRoot, nextRoot);
        bool resetAnalysis = analysisInvalidated || codeIdentityChanged;
        if (!resetAnalysis && finder != nullptr &&
            ShouldValidateCoordinatePoolCode(
                targetFrame,
                nextCodeValidationFrame,
                codeValidationRequested)) {
            const CodeValidationResult validation =
                ValidateCodeFingerprintsUnlocked();
            codeValidationRequested = false;
            if (validation == CodeValidationResult::Changed) {
                analysisInvalidated = true;
                resetAnalysis = true;
            } else {
                nextCodeValidationFrame =
                    NextCoordinatePoolCodeValidationFrame(
                        targetFrame,
                        validation == CodeValidationResult::Unchanged);
                if (validation == CodeValidationResult::Unavailable) {
                    ClearReadDiagnosticUnlocked();
                }
            }
        }
        if (resetAnalysis) ResetAnalysisUnlocked();
        if (!resetAnalysis && runtimeContextChanged &&
            !InvalidateRuntimeContextUnlocked()) {
            SetError(CoordinatePoolRuntimeError::EngineSetupFailed);
            return false;
        }

        bridge = nextRoot.bridge;
        decryptContext = nextRoot.context;
        guestEntry = nextRoot.entry;
        probe.bridge = bridge;
        probe.context = decryptContext;
        probe.guestEntry = guestEntry;
        probe.stage = CoordinatePoolRuntimeStage::RootResolved;

        if (finder == nullptr) {
            if (!AnalyzeCodeUnlocked()) return false;
            nextCodeValidationFrame = NextCoordinatePoolCodeValidationFrame(
                targetFrame, true);
            codeValidationRequested = false;
        }

        probe.threadId = targetExecutionContext.threadId;
        probe.contextGeneration = targetExecutionContext.generation;
        if (!targetExecutionContext.IsUsable()) {
            executionContext = {};
            static_cast<void>(InvalidateRuntimeContextUnlocked());
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
            if (!InvalidateRuntimeContextUnlocked()) {
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
        ClearReadDiagnosticUnlocked();
        probe.error = CoordinatePoolRuntimeError::None;
        probe.stage = CoordinatePoolRuntimeStage::ContextReady;
        return true;
    }

    bool ReadPosition(std::uintptr_t component,
                      CoordinatePoolPosition& position) {
        std::lock_guard<std::mutex> lock(mutex);
        ClearReadDiagnosticUnlocked();
        position = {};
        ++probe.attempts;
        if (memory == nullptr || finder == nullptr || engine == nullptr ||
            !executionContext.IsUsable() || !IsValidGuestAddress(component)) {
            SetError(CoordinatePoolRuntimeError::InvalidInput);
            return false;
        }
        const bool validationWasRequested = codeValidationRequested;
        codeValidationRequested = true;
        if (!parametersReady && !PrepareParametersUnlocked(component)) {
            return false;
        }
        RefreshPoolPointerUnlocked();

        RingSlot* selected = nullptr;
        auto found = ringSlots.find(component);
        const bool hasRing = found != ringSlots.end() &&
            found->second.ring != 0;
        const bool retryMissing = found == ringSlots.end() ||
            (!hasRing && ShouldRetryCoordinatePoolRing(
                found->second.stamp, frame));
        const bool refreshStale = hasRing &&
            ShouldRefreshCoordinatePoolRing(
                component,
                found->second.stamp,
                frame,
                layout.ringRefreshFrames);
        if ((retryMissing || refreshStale) &&
            ringSearchBudget.TryConsume(frame)) {
            std::uint64_t ring = 0;
            CoordinatePoolRuntimeError ringError =
                CoordinatePoolRuntimeError::RingSearchFailed;
            if (ExecuteRingSearchUnlocked(component, ring, ringError)) {
                RingSlot& slot = ringSlots[component];
                slot.ring = ring;
                slot.stamp = frame;
                selected = &slot;
            } else if (hasRing) {
                selected = &found->second;
            } else {
                RingSlot& slot = ringSlots[component];
                slot.ring = 0;
                slot.stamp = frame;
                SetError(ringError);
                return false;
            }
        } else if (hasRing) {
            selected = &found->second;
        } else {
            SetError(CoordinatePoolRuntimeError::RingSearchFailed);
            return false;
        }

        const std::uint64_t indexAddress = NormalizePointer(
            selected->ring + static_cast<std::uint64_t>(finder->index_offset));
        if (!IsRemoteAddress(indexAddress)) {
            SetError(CoordinatePoolRuntimeError::PositionReadFailed);
            return false;
        }

        std::uint64_t index = 0;
        std::uint64_t indexAfter = 0;
        CoordinatePoolPosition candidate{};
        bool stable = false;
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (!ReadRemoteUnlocked(
                    indexAddress,
                    &index,
                    sizeof(index),
                    CoordinateReadStage::RingIndex)) {
                break;
            }
            const std::uint64_t poolSlot = finder->decode_ring_slot(index);
            if (poolSlot >
                (std::numeric_limits<std::uint64_t>::max() -
                    selected->ring - finder->get_ring_offset() -
                    layout.poolHeadSkip) /
                    layout.entryStride) {
                break;
            }
            const std::uint64_t coordinateAddress = NormalizePointer(
                selected->ring + finder->get_ring_offset() +
                layout.poolHeadSkip +
                static_cast<std::uint64_t>(layout.entryStride) * poolSlot);
            if (!IsRemoteAddress(coordinateAddress) ||
                !ReadRemoteUnlocked(
                    coordinateAddress,
                    &candidate,
                    sizeof(candidate),
                    CoordinateReadStage::Position) ||
                !ReadRemoteUnlocked(
                    indexAddress,
                    &indexAfter,
                    sizeof(indexAfter),
                    CoordinateReadStage::RingIndex)) {
                break;
            }
            if (index == indexAfter) {
                stable = true;
                break;
            }
        }
        if (!stable) {
            selected->ring = 0;
            selected->stamp = frame;
            SetError(
                probe.read.HasFailure()
                    ? CoordinatePoolRuntimeError::PositionReadFailed
                    : CoordinatePoolRuntimeError::PositionUnstable);
            return false;
        }
        if (!IsFinitePosition(candidate)) {
            selected->ring = 0;
            selected->stamp = frame;
            SetError(CoordinatePoolRuntimeError::PositionNotFinite);
            return false;
        }

        position = candidate;
        ++probe.successes;
        codeValidationRequested = validationWasRequested;
        ClearReadDiagnosticUnlocked();
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
        try {
            const bool loaded = self->LoadRemotePageUnlocked(address);
            if (!loaded) self->hookFailed = true;
            return loaded;
        } catch (...) {
            self->hookFailed = true;
            return false;
        }
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

    bool ReadRootSnapshotUnlocked(CoordinatePoolRootSnapshot& snapshot) {
        snapshot = {};
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
                sizeof(rawBridge),
                CoordinateReadStage::Root)) {
            return false;
        }
        const std::uint64_t nextBridge = NormalizePointer(rawBridge);
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
                contextAddress,
                &rawContext,
                sizeof(rawContext),
                CoordinateReadStage::Context) ||
            !ReadRemoteUnlocked(
                nextBridge + layout.entryOffset,
                &rawEntry,
                sizeof(rawEntry),
                CoordinateReadStage::Entry)) {
            return false;
        }
        snapshot = {
            nextBridge,
            rawContext,
            NormalizePointer(rawEntry),
        };
        return IsValidGuestAddress(snapshot.context) &&
            IsRemoteAddress(snapshot.entry) && (snapshot.entry & 3U) == 0;
    }

    bool ResolveRootUnlocked(CoordinatePoolRootSnapshot& snapshot,
                             int& status) {
        snapshot = {};
        status = 0;
        CoordinatePoolRootStabilityWindow stability;
        for (std::size_t attempt = 0;
             attempt < kRootSnapshotReadLimit;
             ++attempt) {
            CoordinatePoolRootSnapshot current{};
            if (!ReadRootSnapshotUnlocked(current)) {
                stability.Reset();
                continue;
            }
            if (stability.Observe(current)) {
                snapshot = current;
                ClearReadDiagnosticUnlocked();
                return true;
            }
        }
        status = probe.read.HasFailure() ? probe.systemError : -EAGAIN;
        return false;
    }

    bool AnalyzeCodeUnlocked() {
        constexpr std::size_t kMaximumSnapshotAttempts = 2;
        for (std::size_t attempt = 0;
             attempt < kMaximumSnapshotAttempts;
             ++attempt) {
            analysisInvalidated = false;
            if (AnalyzeCodeAttemptUnlocked()) return true;
            if (!analysisInvalidated ||
                attempt + 1 == kMaximumSnapshotAttempts) {
                return false;
            }
            analysisCodeFingerprints.clear();
            ClearReadDiagnosticUnlocked();
        }
        return false;
    }

    bool AnalyzeCodeAttemptUnlocked() {
        std::uint64_t mappingStart = 0;
        std::uint64_t mappingEnd = 0;
        int mappingStatus = 0;
        if (!FindExecutableMapping(
                processId,
                guestEntry,
                mappingStart,
                mappingEnd,
                &mappingStatus)) {
            SetError(
                CoordinatePoolRuntimeError::EntryMappingMissing,
                true,
                mappingStatus);
            return false;
        }
        if (mappingEnd <= mappingStart ||
            mappingEnd - mappingStart > kMaximumCodeSize ||
            (mappingEnd - mappingStart) % kPageSize != 0 ||
            guestEntry < mappingStart || guestEntry >= mappingEnd ||
            (guestEntry & 3U) != 0) {
            SetError(CoordinatePoolRuntimeError::AnalysisFailed);
            return false;
        }

        const std::size_t mappingSize = static_cast<std::size_t>(
            mappingEnd - mappingStart);
        // FindDec keeps one address-indexed Capstone array. Materialize that
        // array with NOP placeholders, then replace only pages reached by the
        // entry/decode scans. Placeholders are never copied into Unicorn.
        std::vector<std::uint8_t> bytes(mappingSize);
        for (std::size_t offset = 0; offset + sizeof(kArm64Nop) <= bytes.size();
             offset += sizeof(kArm64Nop)) {
            std::memcpy(bytes.data() + offset, &kArm64Nop, sizeof(kArm64Nop));
        }
        const std::size_t pageCount = mappingSize / kPageSize;
        std::vector<bool> loadedPages(pageCount, false);
        std::vector<CodeRangeFingerprint> rangeFingerprints;
        std::size_t loadedPageCount = 0;

        auto failCodeRead = [&](CoordinateReadDiagnostic diagnostic) {
            if (diagnostic.failure ==
                CoordinateReadFailure::MappingChanged) {
                analysisInvalidated = true;
            }
            probe.read = diagnostic;
            SetError(
                CoordinatePoolRuntimeError::CodeReadFailed,
                true,
                diagnostic.systemError);
            return false;
        };

        auto loadPage = [&](std::uint64_t pageAddress) {
            if (pageAddress < mappingStart || pageAddress >= mappingEnd ||
                (pageAddress & (kPageSize - 1)) != 0) {
                return false;
            }
            const std::size_t pageIndex = static_cast<std::size_t>(
                (pageAddress - mappingStart) / kPageSize);
            if (pageIndex >= loadedPages.size() || loadedPages[pageIndex]) {
                return true;
            }

            std::array<std::uint8_t, kPageSize> pageData{};
            CoordinateReadDiagnostic firstRead{};
            if (!ReadRemoteUnlocked(
                    pageAddress,
                    pageData.data(),
                    pageData.size(),
                    CoordinateReadStage::CodePage,
                    &firstRead)) {
                std::uint64_t refreshedStart = 0;
                std::uint64_t refreshedEnd = 0;
                int refreshStatus = 0;
                const bool mappingFound = FindExecutableMapping(
                    processId,
                    guestEntry,
                    refreshedStart,
                    refreshedEnd,
                    &refreshStatus);
                if (mappingFound &&
                    (refreshedStart != mappingStart ||
                     refreshedEnd != mappingEnd)) {
                    firstRead.failure = CoordinateReadFailure::MappingChanged;
                    firstRead.systemError = -ESTALE;
                } else if (!mappingFound && refreshStatus == -ENOENT) {
                    firstRead.failure = CoordinateReadFailure::MappingChanged;
                    firstRead.systemError = -ESTALE;
                } else if (mappingFound) {
                    CoordinateReadDiagnostic retryRead{};
                    if (ReadRemoteUnlocked(
                            pageAddress,
                            pageData.data(),
                            pageData.size(),
                            CoordinateReadStage::CodePage,
                            &retryRead)) {
                        ClearReadDiagnosticUnlocked();
                        firstRead = {};
                    } else {
                        retryRead.attemptedPaths |= firstRead.attemptedPaths;
                        retryRead.attemptCount += firstRead.attemptCount;
                        retryRead.primaryPath = firstRead.primaryPath;
                        retryRead.primaryCompleted = firstRead.primaryCompleted;
                        retryRead.primarySystemError = firstRead.primarySystemError;
                        firstRead = retryRead;
                    }
                }
                if (firstRead.failure != CoordinateReadFailure::None) {
                    return failCodeRead(firstRead);
                }
            }

            const std::size_t byteOffset = pageIndex * kPageSize;
            std::memcpy(bytes.data() + byteOffset, pageData.data(), pageData.size());
            loadedPages[pageIndex] = true;
            ++loadedPageCount;
            return true;
        };

        auto recordMethodRange = [&](std::uint64_t methodAddress,
                                     std::uint64_t returnAddress) {
            if (returnAddress < methodAddress ||
                returnAddress > mappingEnd - 4) {
                return false;
            }
            const std::uint64_t rangeEnd = returnAddress + 4;
            const std::size_t size = static_cast<std::size_t>(
                rangeEnd - methodAddress);
            const std::size_t offset = static_cast<std::size_t>(
                methodAddress - mappingStart);
            const auto existing = std::find_if(
                rangeFingerprints.begin(),
                rangeFingerprints.end(),
                [methodAddress, size](const CodeRangeFingerprint& range) {
                    return range.remoteAddress == methodAddress &&
                        range.size == size;
                });
            if (existing == rangeFingerprints.end()) {
                rangeFingerprints.push_back(CodeRangeFingerprint{
                    methodAddress,
                    size,
                    CoordinatePoolCodeFingerprint(bytes.data() + offset, size),
                });
            }
            return true;
        };

        auto loadMethod = [&](std::uint64_t methodAddress,
                              std::size_t instructionLimit,
                              EntryCodeScan& scan) {
            if (methodAddress < mappingStart || methodAddress >= mappingEnd ||
                (methodAddress & 3U) != 0 || instructionLimit == 0 ||
                instructionLimit >
                    (std::numeric_limits<std::uint64_t>::max() / 4U)) {
                return false;
            }
            const std::uint64_t maximumBytes =
                static_cast<std::uint64_t>(instructionLimit) * 4U;
            if (methodAddress > std::numeric_limits<std::uint64_t>::max() -
                    maximumBytes) {
                return false;
            }
            const std::uint64_t scanLimitEnd = std::min(
                mappingEnd, methodAddress + maximumBytes);
            std::uint64_t pageAddress = methodAddress & kPageMask;
            while (pageAddress < scanLimitEnd) {
                if (!loadPage(pageAddress)) return false;
                const std::uint64_t pageEnd = std::min(
                    mappingEnd, pageAddress + kPageSize);
                const std::uint64_t scanEnd = std::min(pageEnd, scanLimitEnd);
                if (!ScanEntryCode(
                        bytes,
                        mappingStart,
                        methodAddress,
                        scanEnd,
                        scan)) {
                    return false;
                }
                if (scan.returnAddress != 0) {
                    return recordMethodRange(
                        methodAddress, scan.returnAddress);
                }
                if (scanEnd >= scanLimitEnd) break;
                pageAddress += kPageSize;
            }
            return false;
        };

        EntryCodeScan entryScan{};
        if (!loadMethod(
                guestEntry,
                kEntryAnalysisInstructionLimit,
                entryScan)) {
            if (!probe.read.HasFailure()) {
                SetError(CoordinatePoolRuntimeError::AnalysisFailed);
            }
            return false;
        }

        std::unique_ptr<pool::coord_dec::FindDec> candidate;
        bool analysisComplete = false;
        constexpr std::size_t kMaximumAnalysisPasses = 8;
        for (std::size_t pass = 0; pass < kMaximumAnalysisPasses; ++pass) {
            candidate = std::unique_ptr<pool::coord_dec::FindDec>(
                new (std::nothrow) pool::coord_dec::FindDec());
            if (candidate == nullptr ||
                candidate->set(
                    mappingStart,
                    bytes.data(),
                    static_cast<std::uint32_t>(bytes.size())) != 0) {
                SetError(CoordinatePoolRuntimeError::AnalysisFailed);
                return false;
            }

            const int result = candidate->find_dec(guestEntry);
            const auto requestedMethods =
                candidate->get_shellcode()->requested_method_addresses();
            const std::size_t loadedBefore = loadedPageCount;
            for (const std::uint64_t requestedAddress : requestedMethods) {
                const std::size_t limit = requestedAddress == guestEntry
                    ? kEntryAnalysisInstructionLimit
                    : kDecodeAnalysisInstructionLimit;
                EntryCodeScan methodScan{};
                if (!loadMethod(requestedAddress, limit, methodScan) &&
                    probe.read.HasFailure()) {
                    return false;
                }
            }
            if (loadedPageCount != loadedBefore) continue;
            if (result != 0) {
                SetError(CoordinatePoolRuntimeError::AnalysisFailed);
                return false;
            }
            analysisComplete = true;
            break;
        }
        if (!analysisComplete || candidate == nullptr) {
            SetError(CoordinatePoolRuntimeError::AnalysisFailed);
            return false;
        }
        if (rangeFingerprints.empty()) {
            SetError(CoordinatePoolRuntimeError::AnalysisFailed);
            return false;
        }
        const CodeValidationResult snapshotValidation =
            ValidateCodeFingerprintsUnlocked(rangeFingerprints);
        if (snapshotValidation != CodeValidationResult::Unchanged) {
            if (snapshotValidation == CodeValidationResult::Changed) {
                analysisInvalidated = true;
            }
            SetError(
                CoordinatePoolRuntimeError::CodeReadFailed,
                true,
                probe.read.HasFailure() ? probe.read.systemError : -ESTALE);
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
        analysisCodeFingerprints = std::move(rangeFingerprints);
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
        ClearReadDiagnosticUnlocked();
        probe.error = CoordinatePoolRuntimeError::None;
        probe.stage = CoordinatePoolRuntimeStage::CodeAnalyzed;
        return true;
    }

    CodeValidationResult ValidateCodeFingerprintUnlocked(
        const CodeRangeFingerprint& expected) {
        if (expected.size == 0 ||
            expected.remoteAddress >
                std::numeric_limits<std::uint64_t>::max() - expected.size) {
            return CodeValidationResult::Changed;
        }
        std::vector<std::uint8_t> data(expected.size);
        CoordinateReadDiagnostic diagnostic{};
        if (!ReadRemoteUnlocked(
                expected.remoteAddress,
                data.data(),
                data.size(),
                CoordinateReadStage::CodePage,
                &diagnostic)) {
            return CodeValidationResult::Unavailable;
        }
        if (CoordinatePoolCodeFingerprint(data.data(), data.size()) ==
            expected.fingerprint) {
            return CodeValidationResult::Unchanged;
        }

        diagnostic = {};
        diagnostic.stage = CoordinateReadStage::CodePage;
        diagnostic.failure = CoordinateReadFailure::MappingChanged;
        diagnostic.address = expected.remoteAddress;
        diagnostic.size = data.size();
        diagnostic.systemError = -ESTALE;
        probe.read = diagnostic;
        probe.systemError = diagnostic.systemError;
        return CodeValidationResult::Changed;
    }

    CodeValidationResult ValidateCodeFingerprintsUnlocked(
        const std::vector<CodeRangeFingerprint>& fingerprints) {
        bool unavailable = false;
        for (const CodeRangeFingerprint& fingerprint : fingerprints) {
            const CodeValidationResult result =
                ValidateCodeFingerprintUnlocked(fingerprint);
            if (result == CodeValidationResult::Changed) return result;
            unavailable = unavailable ||
                result == CodeValidationResult::Unavailable;
        }
        return unavailable
            ? CodeValidationResult::Unavailable
            : CodeValidationResult::Unchanged;
    }

    CodeValidationResult ValidateCodeFingerprintsUnlocked() {
        return ValidateCodeFingerprintsUnlocked(analysisCodeFingerprints);
    }

    static bool RequiresInstructionHook(std::uint32_t instruction) noexcept {
        return (instruction & kPacgaMask) == kPacgaOpcode ||
            (instruction & kSvcMask) == kSvcOpcode;
    }

    bool RemoveInstructionHooksUnlocked(
        std::vector<uc_hook>& hooks) noexcept {
        bool removed = true;
        if (engine != nullptr) {
            for (const uc_hook hook : hooks) {
                removed = uc_hook_del(engine, hook) == UC_ERR_OK && removed;
            }
        }
        hooks.clear();
        return removed;
    }

    bool InstallInstructionHooksUnlocked(
        std::uint64_t guestBase,
        const std::uint8_t* bytes,
        std::size_t size,
        bool includeParameterCaptures,
        std::vector<uc_hook>& hooks) {
        if (engine == nullptr || bytes == nullptr || size < 4 ||
            guestBase > std::numeric_limits<std::uint64_t>::max() - size) {
            return false;
        }

        std::vector<std::uint64_t> addresses;
        addresses.reserve(size / 256 + 8);
        for (std::size_t offset = 0; offset <= size - 4; offset += 4) {
            std::uint32_t instruction = 0;
            std::memcpy(&instruction, bytes + offset, sizeof(instruction));
            if (RequiresInstructionHook(instruction)) {
                addresses.push_back(guestBase + offset);
            }
        }
        if (includeParameterCaptures) {
            for (const auto& parameter : finder->analyze.varParams) {
                if (parameter.addr != 0 && (parameter.addr & 3U) == 0 &&
                    parameter.addr >= guestBase &&
                    parameter.addr - guestBase < size) {
                    addresses.push_back(parameter.addr);
                }
            }
        }
        std::sort(addresses.begin(), addresses.end());
        addresses.erase(
            std::unique(addresses.begin(), addresses.end()),
            addresses.end());

        const std::size_t initialSize = hooks.size();
        hooks.reserve(initialSize + addresses.size());
        for (const std::uint64_t address : addresses) {
            uc_hook hook = 0;
            if (uc_hook_add(
                    engine,
                    &hook,
                    UC_HOOK_CODE,
                    reinterpret_cast<void*>(CodeHook),
                    this,
                    address,
                    address) != UC_ERR_OK) {
                while (hooks.size() > initialSize) {
                    static_cast<void>(uc_hook_del(engine, hooks.back()));
                    hooks.pop_back();
                }
                return false;
            }
            hooks.push_back(hook);
        }
        return true;
    }

    void ApplyPatchedCodePageUnlocked(
        std::uint64_t guestPage,
        std::array<std::uint8_t, kPageSize>& data) {
        if (finder == nullptr || codeBase == 0 || codeSize == 0 ||
            guestPage < codeBase || guestPage - codeBase >= codeSize) {
            return;
        }
        auto* shellcode = finder->get_shellcode();
        if (shellcode == nullptr) return;
        // Unread snapshot regions contain NOP placeholders. Only explicit
        // FindDec patches may replace bytes fetched from the remote process.
        static_cast<void>(shellcode->apply_patches(
            guestPage, data.data(), data.size()));
    }

    bool LoadCodePageUnlocked(std::uint64_t guestPage) {
        if (engine == nullptr || memory == nullptr || finder == nullptr ||
            codeBase == 0 || codeSize == 0 || guestPage < codeBase ||
            guestPage - codeBase >= codeSize) {
            return false;
        }
        const auto existing = std::find_if(
            codePages.begin(),
            codePages.end(),
            [guestPage](const CachedPage& page) {
                return page.guestAddress == guestPage;
            });
        if (existing != codePages.end()) return true;

        const std::uint64_t remotePage = NormalizePointer(guestPage) & kPageMask;
        if (!IsRemoteAddress(remotePage) ||
            remotePage > kMaximumRemoteAddress - kPageSize) {
            return false;
        }

        std::array<std::uint8_t, kPageSize> data{};
        CoordinateReadDiagnostic firstRead{};
        if (!ReadRemoteUnlocked(
                remotePage,
                data.data(),
                data.size(),
                CoordinateReadStage::CodePage,
                &firstRead)) {
            std::uint64_t refreshedStart = 0;
            std::uint64_t refreshedEnd = 0;
            int refreshStatus = 0;
            const bool mappingFound = FindExecutableMapping(
                processId,
                guestEntry,
                refreshedStart,
                refreshedEnd,
                &refreshStatus);
            if (mappingFound &&
                (refreshedStart != codeBase ||
                 refreshedEnd - refreshedStart != codeSize)) {
                firstRead.failure = CoordinateReadFailure::MappingChanged;
                firstRead.systemError = -ESTALE;
            } else if (!mappingFound && refreshStatus == -ENOENT) {
                firstRead.failure = CoordinateReadFailure::MappingChanged;
                firstRead.systemError = -ESTALE;
            } else if (mappingFound) {
                CoordinateReadDiagnostic retryRead{};
                if (ReadRemoteUnlocked(
                        remotePage,
                        data.data(),
                        data.size(),
                        CoordinateReadStage::CodePage,
                        &retryRead)) {
                    ClearReadDiagnosticUnlocked();
                    firstRead = {};
                } else {
                    retryRead.attemptedPaths |= firstRead.attemptedPaths;
                    retryRead.attemptCount += firstRead.attemptCount;
                    retryRead.primaryPath = firstRead.primaryPath;
                    retryRead.primaryCompleted = firstRead.primaryCompleted;
                    retryRead.primarySystemError = firstRead.primarySystemError;
                    firstRead = retryRead;
                }
            }
            if (firstRead.failure != CoordinateReadFailure::None) {
                if (firstRead.failure == CoordinateReadFailure::MappingChanged) {
                    analysisInvalidated = true;
                }
                probe.read = firstRead;
                probe.systemError = firstRead.systemError;
                return false;
            }
        }
        ApplyPatchedCodePageUnlocked(guestPage, data);

        if (uc_mem_map(engine, guestPage, kPageSize, UC_PROT_ALL) !=
            UC_ERR_OK) {
            return false;
        }
        if (uc_mem_write(engine, guestPage, data.data(), data.size()) !=
            UC_ERR_OK) {
            static_cast<void>(uc_mem_unmap(engine, guestPage, kPageSize));
            return false;
        }

        const std::size_t initialHookCount = codeHooks.size();
        if (!InstallInstructionHooksUnlocked(
                guestPage,
                data.data(),
                data.size(),
                true,
                codeHooks)) {
            while (codeHooks.size() > initialHookCount) {
                static_cast<void>(uc_hook_del(engine, codeHooks.back()));
                codeHooks.pop_back();
            }
            static_cast<void>(uc_mem_unmap(engine, guestPage, kPageSize));
            return false;
        }

        try {
            codePages.push_back(CachedPage{guestPage, remotePage, {}});
        } catch (...) {
            while (codeHooks.size() > initialHookCount) {
                static_cast<void>(uc_hook_del(engine, codeHooks.back()));
                codeHooks.pop_back();
            }
            static_cast<void>(uc_mem_unmap(engine, guestPage, kPageSize));
            return false;
        }
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
        if (uc_mem_map(
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
                        address,
                        &value,
                        parameter.size,
                        CoordinateReadStage::Parameter)) {
                    return false;
                }
                parameter.value = value;
                return true;
            }
            std::uint64_t next = 0;
            if (!ReadRemoteUnlocked(
                    address,
                    &next,
                    sizeof(next),
                    CoordinateReadStage::Parameter)) {
                return false;
            }
            pointer = NormalizePointer(next);
            if (!IsRemoteAddress(pointer)) return false;
        }
        return false;
    }

    bool InvalidateRuntimeContextUnlocked() {
        parametersReady = false;
        computedContext = 0;
        ClearPoolPointerUnlocked();
        poolPointerRefreshFrame =
            std::numeric_limits<std::uint64_t>::max();
        return ClearDynamicPagesUnlocked();
    }

    void ClearPoolPointerUnlocked() {
        lastPoolPointer = 0;
        ringSlots.clear();
        ringSearchBudget.Reset();
        probe.poolPointer = {};
    }

    void RefreshPoolPointerUnlocked() {
        if (poolPointerRefreshFrame == frame) return;
        poolPointerRefreshFrame = frame;

        CoordinatePoolPointerDiagnostic& diagnostic = probe.poolPointer;
        diagnostic = {};
        diagnostic.computedContext = computedContext;
        if (finder != nullptr) {
            diagnostic.stateFlags |= kPoolPointerFinderReady;
            diagnostic.offset = finder->pool_ptr_offset;
        }
        if (parametersReady) {
            diagnostic.stateFlags |= kPoolPointerParametersReady;
        }
        if (computedContext != 0) {
            diagnostic.stateFlags |= kPoolPointerContextReady;
        }
        if (finder != nullptr && finder->pool_ptr_offset != 0) {
            diagnostic.stateFlags |= kPoolPointerOffsetReady;
        }
        if (finder == nullptr || !parametersReady || computedContext == 0) {
            diagnostic.error = CoordinateDecryptError::PoolPointerStateInvalid;
            diagnostic.systemError = -EAGAIN;
            return;
        }
        if (finder->pool_ptr_offset == 0) {
            diagnostic.error =
                CoordinateDecryptError::PoolPointerOffsetMissing;
            diagnostic.systemError = -ENODATA;
            return;
        }

        std::uint64_t rawAddress = 0;
        if (!AddSignedOffset(
                computedContext, finder->pool_ptr_offset, rawAddress)) {
            diagnostic.error =
                CoordinateDecryptError::PoolPointerAddressInvalid;
            diagnostic.systemError = -ERANGE;
            return;
        }
        const std::uint64_t address = NormalizePointer(rawAddress);
        diagnostic.address = address;
        if (!IsRemoteAddress(address)) {
            diagnostic.error =
                CoordinateDecryptError::PoolPointerAddressInvalid;
            diagnostic.systemError = -ERANGE;
            return;
        }

        std::uint64_t rawPointer = 0;
        CoordinateReadDiagnostic readDiagnostic{};
        const CoordinateReadDiagnostic previousRead = probe.read;
        const int previousSystemError = probe.systemError;
        const bool read = ReadRemoteUnlocked(
            address,
            &rawPointer,
            sizeof(rawPointer),
            CoordinateReadStage::PoolPointer,
            &readDiagnostic);
        probe.read = previousRead;
        probe.systemError = previousSystemError;
        diagnostic.rawValue = rawPointer;
        diagnostic.normalizedValue = NormalizePointer(rawPointer);
        if (!read) {
            diagnostic.error = CoordinateDecryptError::PoolPointerReadFailed;
            diagnostic.systemError = readDiagnostic.systemError;
            diagnostic.read = readDiagnostic;
            return;
        }
        if (rawPointer == 0) {
            diagnostic.error =
                CoordinateDecryptError::PoolPointerValueInvalid;
            diagnostic.systemError = -ENODATA;
            return;
        }

        const std::uint64_t pointer = diagnostic.normalizedValue != 0
            ? diagnostic.normalizedValue
            : rawPointer;
        if (ShouldClearCoordinatePoolRingsAfterPointerRefresh(
                true, lastPoolPointer, pointer)) {
            ringSlots.clear();
            ringSearchBudget.Reset();
        }
        lastPoolPointer = pointer;
    }

    bool ExecuteRingSearchUnlocked(
        std::uint64_t component,
        std::uint64_t& ring,
        CoordinatePoolRuntimeError& error) {
        ring = 0;
        error = CoordinatePoolRuntimeError::RingSearchFailed;
        if (layout.componentKeyOffset >
            std::numeric_limits<std::uint64_t>::max() - component ||
            !PrepareRegistersUnlocked(
                decryptContext,
                component + layout.componentKeyOffset,
                true)) {
            error = CoordinatePoolRuntimeError::RingPreparationFailed;
            return false;
        }
        if (!RunStageUnlocked(
                searchEnd,
                kSearchInstructionBudget,
                kSearchTimeout)) {
            error = CoordinatePoolRuntimeError::RingExecutionFailed;
            return false;
        }
        const int registerId = CapstoneXRegisterId(searchRegister);
        std::uint64_t rawRing = 0;
        if (registerId == UC_ARM64_REG_INVALID ||
            uc_reg_read(engine, registerId, &rawRing) != UC_ERR_OK) {
            error = CoordinatePoolRuntimeError::RingRegisterReadFailed;
            return false;
        }
        ring = NormalizePointer(rawRing);
        if (!IsRemoteAddress(ring)) {
            error = CoordinatePoolRuntimeError::RingValueInvalid;
            return false;
        }
        error = CoordinatePoolRuntimeError::None;
        return true;
    }

    bool ReadRemoteUnlocked(
        std::uint64_t address,
        void* destination,
        std::size_t size,
        CoordinateReadStage stage,
        CoordinateReadDiagnostic* result = nullptr) {
        const std::uint64_t normalized = NormalizePointer(address);
        CoordinateReadDiagnostic diagnostic{};
        bool read = false;
        if (memory != nullptr) {
            read = memory->ReadCoordinateMemory(
                normalized, destination, size, diagnostic);
        } else {
            diagnostic.address = normalized;
            diagnostic.size = size;
            diagnostic.failure =
                CoordinateReadFailure::TransportUnavailable;
            diagnostic.systemError = -ENODEV;
        }
        diagnostic.stage = stage;
        if (result != nullptr) *result = diagnostic;
        if (!read) {
            probe.read = diagnostic;
            probe.systemError = diagnostic.systemError;
        }
        return read;
    }

    bool LoadRemotePageUnlocked(std::uint64_t faultAddress) {
        if (engine == nullptr || memory == nullptr) return false;
        const std::uint64_t guestPage = faultAddress & kPageMask;
        if ((guestPage >= kStackBase && guestPage < kStackTop) ||
            guestPage == kArgumentPage) {
            return true;
        }
        if (codeBase != 0 && codeSize != 0 && guestPage >= codeBase &&
            guestPage - codeBase < codeSize) {
            return LoadCodePageUnlocked(guestPage);
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
            ReadRemoteUnlocked(
                remotePage,
                data.data(),
                data.size(),
                CoordinateReadStage::DynamicPage);
        if (!remoteReadable && !allowMissingRemotePages) {
            return false;
        }
        if (!remoteReadable) {
            if (!toleratedReadFailure.HasFailure()) {
                toleratedReadFailure = probe.read;
            }
            probe.systemError = 0;
            probe.read = {};
        }
        if (uc_mem_map(engine, guestPage, kPageSize, UC_PROT_ALL) != UC_ERR_OK) {
            return false;
        }
        if (uc_mem_write(engine, guestPage, data.data(), data.size()) !=
            UC_ERR_OK) {
            static_cast<void>(uc_mem_unmap(engine, guestPage, kPageSize));
            return false;
        }
        CachedPage page{guestPage, remotePage, {}};
        if (!InstallInstructionHooksUnlocked(
                guestPage,
                data.data(),
                data.size(),
                false,
                page.instructionHooks)) {
            static_cast<void>(uc_mem_unmap(engine, guestPage, kPageSize));
            return false;
        }
        try {
            dynamicPages.push_back(std::move(page));
        } catch (...) {
            static_cast<void>(
                RemoveInstructionHooksUnlocked(page.instructionHooks));
            static_cast<void>(uc_mem_unmap(engine, guestPage, kPageSize));
            return false;
        }
        return true;
    }

    bool ClearCodePagesUnlocked() {
        bool cleared = RemoveInstructionHooksUnlocked(codeHooks);
        if (engine == nullptr) {
            codePages.clear();
            return cleared;
        }
        for (const CachedPage& page : codePages) {
            if (uc_mem_unmap(engine, page.guestAddress, kPageSize) !=
                UC_ERR_OK) {
                cleared = false;
            }
        }
        codePages.clear();
        return cleared;
    }

    bool ClearDynamicPagesUnlocked() {
        if (engine == nullptr) {
            dynamicPages.clear();
            return true;
        }
        for (CachedPage& page : dynamicPages) {
            if (!RemoveInstructionHooksUnlocked(page.instructionHooks) ||
                uc_mem_unmap(engine, page.guestAddress, kPageSize) !=
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
        RestoreToleratedReadFailureUnlocked();
        probe.error = error;
        if (!probe.read.HasFailure()) probe.systemError = 0;
        if (faulted) probe.stage = CoordinatePoolRuntimeStage::Faulted;
    }

    void SetError(CoordinatePoolRuntimeError error,
                  bool faulted,
                  int systemError) {
        RestoreToleratedReadFailureUnlocked();
        probe.error = error;
        probe.systemError = systemError;
        if (faulted) probe.stage = CoordinatePoolRuntimeStage::Faulted;
    }

    void ClearReadDiagnosticUnlocked() noexcept {
        probe.systemError = 0;
        probe.read = {};
        toleratedReadFailure = {};
    }

    void RestoreToleratedReadFailureUnlocked() noexcept {
        if (!probe.read.HasFailure() &&
            toleratedReadFailure.HasFailure()) {
            probe.read = toleratedReadFailure;
            probe.systemError = toleratedReadFailure.systemError;
        }
    }

    void ResetAnalysisUnlocked() {
        CloseEngineUnlocked();
        finder.reset();
        analysisCodeFingerprints.clear();
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
        lastPoolPointer = 0;
        poolPointerRefreshFrame =
            std::numeric_limits<std::uint64_t>::max();
        ringSlots.clear();
        ringSearchBudget.Reset();
        probe.bridge = 0;
        probe.context = 0;
        probe.guestEntry = 0;
        probe.codeBase = 0;
        probe.codeSize = 0;
        probe.poolPointerOffset = 0;
        probe.indexOffset = 0;
        probe.ringOffset = 0;
        analysisInvalidated = false;
        nextCodeValidationFrame = 0;
        codeValidationRequested = false;
        ClearReadDiagnosticUnlocked();
        probe.stage = CoordinatePoolRuntimeStage::Idle;
        probe.error = CoordinatePoolRuntimeError::None;
    }

    void CloseEngineUnlocked() {
        if (engine != nullptr) {
            static_cast<void>(ClearCodePagesUnlocked());
            static_cast<void>(ClearDynamicPagesUnlocked());
            static_cast<void>(uc_close(engine));
            engine = nullptr;
        }
        dynamicPages.clear();
        codePages.clear();
        codeHooks.clear();
        memoryHook = 0;
        mrsHook = 0;
        hookFailed = false;
        captureParameters = false;
        allowMissingRemotePages = false;
    }

    void ResetUnlocked() {
        const CoordinatePoolRuntimeLayout configuredLayout = layout;
        CloseEngineUnlocked();
        finder.reset();
        analysisCodeFingerprints.clear();
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
        analysisInvalidated = false;
        nextCodeValidationFrame = 0;
        codeValidationRequested = false;
        frame = 0;
        lastPoolPointer = 0;
        poolPointerRefreshFrame =
            std::numeric_limits<std::uint64_t>::max();
        ringSlots.clear();
        ringSearchBudget.Reset();
        probe = {};
        toleratedReadFailure = {};
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
    bool analysisInvalidated = false;
    std::uint64_t frame = 0;
    CoordinateReadDiagnostic toleratedReadFailure{};
    std::uint64_t lastPoolPointer = 0;
    std::uint64_t poolPointerRefreshFrame =
        std::numeric_limits<std::uint64_t>::max();
    std::unordered_map<std::uint64_t, RingSlot> ringSlots;
    CoordinatePoolRingSearchBudget ringSearchBudget;
    std::unique_ptr<pool::coord_dec::FindDec> finder;
    std::vector<CodeRangeFingerprint> analysisCodeFingerprints;
    std::uint64_t nextCodeValidationFrame = 0;
    bool codeValidationRequested = false;
    uc_engine* engine = nullptr;
    uc_hook memoryHook = 0;
    uc_hook mrsHook = 0;
    std::vector<uc_hook> codeHooks;
    std::vector<CachedPage> codePages;
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
