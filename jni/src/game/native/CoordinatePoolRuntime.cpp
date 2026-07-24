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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef LENGJING_ENABLE_COORDINATE_DEBUG_LOG
#define LENGJING_ENABLE_COORDINATE_DEBUG_LOG 0
#endif

namespace lengjing::game::native {
namespace {

namespace pool = coordinate_pool_internal;

constexpr std::uint64_t kPageSize = 4096;
constexpr std::uint64_t kPageMask = ~(kPageSize - 1);
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
constexpr std::uint64_t kCandidateTraceMinimumInterval = 30;
constexpr std::uint64_t kCandidateTracePeriodicInterval = 60;

std::uint64_t NormalizePointer(std::uint64_t value) noexcept {
    return NormalizeCoordinatePoolPointer(value);
}

bool IsRemoteAddress(std::uint64_t value) noexcept {
    return IsCoordinatePoolReadRangeValid(value, 1);
}

bool IsValidGuestAddress(std::uint64_t value) noexcept {
    return IsRemoteAddress(NormalizePointer(value));
}

CoordinatePoolRuntimeError CoordinateReplayEntryRuntimeError(
    CoordinateDecryptError error,
    const CoordinateReadDiagnostic& read) noexcept {
    using RuntimeError = CoordinatePoolRuntimeError;
    if (read.HasFailure()) {
        switch (read.stage) {
            case CoordinateReadStage::Root:
                return RuntimeError::RootReadFailed;
            case CoordinateReadStage::Entry:
                return RuntimeError::EntryResolveFailed;
            case CoordinateReadStage::CodePage:
                return RuntimeError::EntryPageReadFailed;
            default:
                break;
        }
    }
    switch (error) {
        case CoordinateDecryptError::None:
            return RuntimeError::None;
        case CoordinateDecryptError::InvalidConfiguration:
        case CoordinateDecryptError::MemoryTransportUnavailable:
            return RuntimeError::InvalidInput;
        case CoordinateDecryptError::RootReadFailed:
            return RuntimeError::RootReadFailed;
        case CoordinateDecryptError::EntryResolveFailed:
            return RuntimeError::EntryResolveFailed;
        case CoordinateDecryptError::EntryMappingMissing:
            return RuntimeError::EntryMappingMissing;
        case CoordinateDecryptError::EntryMappingFragmented:
            return RuntimeError::EntryMappingFragmented;
        case CoordinateDecryptError::EntryMappingChanged:
            return RuntimeError::EntryMappingChanged;
        case CoordinateDecryptError::EntryCodePageReadFailed:
            return RuntimeError::EntryPageReadFailed;
        case CoordinateDecryptError::EntryCodeReadFailed:
        case CoordinateDecryptError::EntryCodeReadInvalidRange:
        case CoordinateDecryptError::EntryCodeReadUnavailable:
        case CoordinateDecryptError::EntryCodeReadPermissionDenied:
        case CoordinateDecryptError::EntryCodeReadAddressFault:
        case CoordinateDecryptError::EntryCodeReadShort:
        case CoordinateDecryptError::EntryCodeProcessVmReadFailed:
        case CoordinateDecryptError::EntryCodeProcMemOpenFailed:
        case CoordinateDecryptError::EntryCodeProcMemReadFailed:
            return RuntimeError::EntryCodeReadFailed;
        default:
            return RuntimeError::CodeReadFailed;
    }
}

bool IsCoordinatePoolTraceEnabled() noexcept {
#if LENGJING_ENABLE_COORDINATE_DEBUG_LOG
    static const bool enabled = CoordinatePoolEnvironmentFlagEnabled(
        std::getenv("LENGJING_COORDINATE_TRACE"));
    return enabled;
#else
    return false;
#endif
}

bool IsCoordinatePoolCandidateTraceFullEnabled() noexcept {
#if LENGJING_ENABLE_COORDINATE_DEBUG_LOG
    static const bool enabled = CoordinatePoolEnvironmentFlagEnabled(
        std::getenv("LENGJING_COORDINATE_CANDIDATES_FULL"));
    return enabled;
#else
    return false;
#endif
}

std::uint64_t CoordinatePoolParameterFingerprint(
    const pool::coord_dec::FindDec& finder) noexcept {
    std::uint64_t hash = UINT64_C(1469598103934665603);
    const auto mix = [&hash](std::uint64_t value) {
        hash ^= value;
        hash *= UINT64_C(1099511628211);
    };
    const auto mixText = [&mix](const std::string& value) {
        for (const unsigned char character : value) mix(character);
        mix(0);
    };
    for (const auto& parameter : finder.mem_param_list) {
        mixText(parameter.name);
        mix(parameter.size);
        mix(static_cast<std::uint32_t>(parameter.disp));
        mix(parameter.value);
        for (const std::int32_t offset : parameter.offset) {
            mix(static_cast<std::uint32_t>(offset));
        }
        mix(0xA5U);
    }
    for (const auto& parameter : finder.analyze.varParams) {
        mixText(parameter.name);
        mix(parameter.addr);
        mix(static_cast<std::uint32_t>(parameter.reg));
        mix(parameter.value);
        mix(0x5AU);
    }
    return hash;
}

struct ExecutableMappingSegment {
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    std::uint64_t fileOffset = 0;
    std::uint64_t inode = 0;
    std::string device;
    std::string path;

    bool Contains(std::uint64_t address) const noexcept {
        return address >= start && address < end;
    }
};

struct ExecutableMappingIndex {
    std::vector<ExecutableMappingSegment> segments;
    std::uint64_t windowStart = 0;
    std::uint64_t windowEnd = 0;
    std::uint64_t entrySegmentStart = 0;
    std::uint64_t entrySegmentEnd = 0;
    std::size_t fragmentCount = 0;

    bool Contains(std::uint64_t address) const noexcept {
        const auto found = std::upper_bound(
            segments.begin(),
            segments.end(),
            address,
            [](std::uint64_t candidate,
               const ExecutableMappingSegment& segment) {
                return candidate < segment.start;
            });
        return found != segments.begin() && std::prev(found)->Contains(address);
    }

    bool ContainsPage(std::uint64_t pageAddress) const noexcept {
        return (pageAddress & (kPageSize - 1)) == 0 &&
            pageAddress <=
                std::numeric_limits<std::uint64_t>::max() - kPageSize &&
            Contains(pageAddress) && Contains(pageAddress + kPageSize - 1);
    }
};

bool SameFileMapping(const ExecutableMappingSegment& left,
                     const ExecutableMappingSegment& right) noexcept {
    return left.inode == right.inode && left.device == right.device &&
        left.path == right.path;
}

bool SameExecutableMappingIndex(const ExecutableMappingIndex& left,
                                const ExecutableMappingIndex& right) noexcept {
    if (left.windowStart != right.windowStart ||
        left.windowEnd != right.windowEnd ||
        left.entrySegmentStart != right.entrySegmentStart ||
        left.entrySegmentEnd != right.entrySegmentEnd ||
        left.fragmentCount != right.fragmentCount ||
        left.segments.size() != right.segments.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.segments.size(); ++index) {
        const ExecutableMappingSegment& first = left.segments[index];
        const ExecutableMappingSegment& second = right.segments[index];
        if (first.start != second.start || first.end != second.end ||
            first.fileOffset != second.fileOffset ||
            !SameFileMapping(first, second)) {
            return false;
        }
    }
    return true;
}

bool ParseExecutableMappingLine(const std::string& line,
                                ExecutableMappingSegment& segment,
                                bool& executable) {
    segment = {};
    executable = false;
    unsigned long long start = 0;
    unsigned long long end = 0;
    unsigned long long fileOffset = 0;
    unsigned long long inode = 0;
    char permissions[5]{};
    char device[64]{};
    char path[1024]{};
    const int parsed = std::sscanf(
        line.c_str(),
        "%llx-%llx %4s %llx %63s %llu %1023[^\n]",
        &start,
        &end,
        permissions,
        &fileOffset,
        device,
        &inode,
        path);
    if (parsed < 6 || end <= start) return false;

    segment.start = start;
    segment.end = end;
    segment.fileOffset = fileOffset;
    segment.inode = inode;
    segment.device = device;
    if (parsed == 7) {
        const std::string rawPath(path);
        const std::size_t first = rawPath.find_first_not_of(" \t");
        if (first != std::string::npos) segment.path = rawPath.substr(first);
    }
    executable = permissions[2] == 'x';
    return true;
}

bool BuildExecutableMappingIndex(pid_t processId,
                                 std::uint64_t address,
                                 ExecutableMappingIndex& index,
                                 int* systemError = nullptr) {
    index = {};
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

    std::vector<ExecutableMappingSegment> executableMappings;
    ExecutableMappingSegment entryMapping{};
    bool foundEntry = false;
    std::string line;
    while (std::getline(maps, line)) {
        ExecutableMappingSegment candidate{};
        bool executable = false;
        if (!ParseExecutableMappingLine(line, candidate, executable) ||
            !executable) {
            continue;
        }
        if (candidate.Contains(address)) {
            entryMapping = candidate;
            foundEntry = true;
        }
        executableMappings.push_back(std::move(candidate));
    }
    if (maps.bad()) {
        if (systemError != nullptr) *systemError = -EIO;
        return false;
    }
    if (!foundEntry) {
        if (systemError != nullptr) *systemError = -ENOENT;
        return false;
    }
    if ((entryMapping.start & (kPageSize - 1)) != 0 ||
        (entryMapping.end & (kPageSize - 1)) != 0 ||
        (entryMapping.fileOffset & (kPageSize - 1)) != 0) {
        if (systemError != nullptr) *systemError = -ERANGE;
        return false;
    }

    std::vector<ExecutableMappingSegment> fileMappings;
    for (ExecutableMappingSegment& candidate : executableMappings) {
        if (!SameFileMapping(entryMapping, candidate)) continue;
        if ((candidate.start & (kPageSize - 1)) != 0 ||
            (candidate.end & (kPageSize - 1)) != 0 ||
            (candidate.fileOffset & (kPageSize - 1)) != 0) {
            if (candidate.Contains(address)) {
                if (systemError != nullptr) *systemError = -ERANGE;
                return false;
            }
            continue;
        }
        fileMappings.push_back(std::move(candidate));
    }
    if (fileMappings.empty()) {
        if (systemError != nullptr) *systemError = -ENOENT;
        return false;
    }
    std::sort(
        fileMappings.begin(),
        fileMappings.end(),
        [](const ExecutableMappingSegment& left,
           const ExecutableMappingSegment& right) {
            return left.start < right.start;
        });
    index.fragmentCount = fileMappings.size();

    for (ExecutableMappingSegment& candidate : fileMappings) {
        if (!index.segments.empty()) {
            ExecutableMappingSegment& previous = index.segments.back();
            const std::uint64_t previousSize = previous.end - previous.start;
            if (previous.end == candidate.start &&
                previous.fileOffset <=
                    std::numeric_limits<std::uint64_t>::max() -
                        previousSize &&
                previous.fileOffset + previousSize == candidate.fileOffset) {
                previous.end = candidate.end;
                continue;
            }
        }
        index.segments.push_back(std::move(candidate));
    }

    const auto containing = std::find_if(
        index.segments.begin(),
        index.segments.end(),
        [address](const ExecutableMappingSegment& segment) {
            return segment.Contains(address);
        });
    if (containing == index.segments.end()) {
        if (systemError != nullptr) *systemError = -ENOENT;
        return false;
    }
    index.entrySegmentStart = containing->start;
    index.entrySegmentEnd = containing->end;

    const std::uint64_t entryPage = address & kPageMask;
    const std::uint64_t segmentSize = containing->end - containing->start;
    if (segmentSize <= kMaximumCodeSize) {
        index.windowStart = containing->start;
        index.windowEnd = containing->end;
    } else {
        constexpr std::uint64_t kWindowHalf = kMaximumCodeSize / 2;
        index.windowStart = entryPage > kWindowHalf
            ? entryPage - kWindowHalf
            : containing->start;
        index.windowStart = std::max(index.windowStart, containing->start);
        index.windowStart &= kPageMask;
        index.windowEnd = index.windowStart + kMaximumCodeSize;
        if (index.windowEnd > containing->end) {
            index.windowEnd = containing->end;
            index.windowStart = std::max(
                containing->start,
                index.windowEnd - kMaximumCodeSize);
        }
    }
    if (index.windowEnd <= index.windowStart ||
        index.windowEnd - index.windowStart > kMaximumCodeSize ||
        (index.windowStart & (kPageSize - 1)) != 0 ||
        (index.windowEnd & (kPageSize - 1)) != 0 ||
        address < index.windowStart || address >= index.windowEnd ||
        !index.ContainsPage(entryPage)) {
        if (systemError != nullptr) *systemError = -ERANGE;
        index = {};
        return false;
    }
    return true;
}

bool AddSignedOffset(std::uint64_t base,
                     std::int64_t offset,
                     std::uint64_t& result) noexcept {
    if (offset >= 0) {
        const auto increment = static_cast<std::uint64_t>(offset);
        if (base > std::numeric_limits<std::uint64_t>::max() - increment) {
            return false;
        }
        result = base + increment;
        return true;
    }
    const auto decrement = static_cast<std::uint64_t>(-(offset + 1)) + 1;
    if (base < decrement) return false;
    result = base - decrement;
    return true;
}

bool AddUnsignedOffset(std::uint64_t base,
                       std::uint64_t offset,
                       std::uint64_t& result) noexcept {
    if (base > std::numeric_limits<std::uint64_t>::max() - offset) {
        return false;
    }
    result = base + offset;
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
    if (value == ARM64_REG_SP) return UC_ARM64_REG_SP;
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
        CoordinatePoolRingRecoveryState recovery{};
        std::uint64_t candidateTraceFrame =
            std::numeric_limits<std::uint64_t>::max();
        std::uint8_t candidateTracePhysicalSlot = UINT8_MAX;
        std::array<CoordinatePoolPosition,
                   kCoordinatePoolPhysicalSlotCount> previousPositions{};
        std::array<bool,
                   kCoordinatePoolPhysicalSlotCount> previousValid{};
        std::uint64_t previousIndex = 0;
        std::uint64_t previousDecodedSlot = 0;
        std::uint8_t previousPhysicalSlotCount = 0;
        bool hasPreviousPositions = false;
        std::array<CoordinatePoolPosition,
                   kCoordinatePoolBlockProbeCount> indexedPositions{};
        std::uint8_t indexedBlockCount = 0;
        std::uint8_t indexedResolvedOffset =
            kCoordinatePoolUnknownDecryptIndexOffset;
        bool hasIndexedPositions = false;
    };

    explicit Impl(CoordinatePoolRuntimeLayout configuredLayout)
        : layout(configuredLayout) {}

    ~Impl() {
        CloseEngineUnlocked();
    }

    std::uint64_t NormalizePointer(std::uint64_t value) const noexcept {
        return indexedPointers
            ? NormalizeCoordinatePoolIndexedPointer(value)
            : NormalizeCoordinatePoolPointer(value);
    }

    bool IsValidGuestAddress(std::uint64_t value) const noexcept {
        return IsRemoteAddress(NormalizePointer(value));
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
                 std::uint64_t targetFrame,
                 bool targetIndexedPointers) {
        std::lock_guard<std::mutex> lock(mutex);
        ClearReadDiagnosticUnlocked();
        if (!layout.IsValid() || targetProcessId <= 0 ||
            !IsRemoteAddress(targetModuleBase)) {
            SetError(CoordinatePoolRuntimeError::InvalidInput);
            return false;
        }

        const bool bindingChanged = memory != &targetMemory ||
            processId != targetProcessId || moduleBase != targetModuleBase;
        const bool pointerModeChanged =
            indexedPointers != targetIndexedPointers;
        if (bindingChanged || pointerModeChanged) {
            const std::uint64_t attempts = probe.attempts;
            const std::uint64_t successes = probe.successes;
            ResetUnlocked();
            probe.attempts = attempts;
            probe.successes = successes;
            memory = &targetMemory;
            processId = targetProcessId;
            moduleBase = targetModuleBase;
            indexedPointers = targetIndexedPointers;
        }

        const CoordinatePoolRootSnapshot previousRoot{
            bridge,
            decryptContext,
            guestEntry,
        };
        CoordinatePoolRootSnapshot nextRoot{};
        int rootStatus = 0;
        if (!ResolveRootUnlocked(previousRoot, nextRoot, rootStatus)) {
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
                      CoordinatePoolPosition& position,
                      bool forceRefresh = false) {
        std::lock_guard<std::mutex> lock(mutex);
        position = {};
        CoordinatePoolCandidateSet candidates{};
        bool validationWasRequested = false;
        if (!ReadCandidatesUnlocked(
                component,
                candidates,
                forceRefresh,
                validationWasRequested,
                false,
                0)) {
            FinishFailedReadUnlocked(validationWasRequested);
            return false;
        }
        if (!IsCoordinatePoolSelectedCandidateValid(candidates)) {
            SetError(CoordinatePoolRuntimeError::PositionNotFinite);
            FinishFailedReadUnlocked(validationWasRequested);
            return false;
        }
        position = candidates.positions[candidates.selectedLogicalSlot];
        FinishReadUnlocked(validationWasRequested);
        return true;
    }

    bool ReadPosition(std::uintptr_t component,
                      std::uint32_t decryptIndexOffset,
                      CoordinatePoolPosition& position,
                      bool forceRefresh = false) {
        std::lock_guard<std::mutex> lock(mutex);
        position = {};
        CoordinatePoolCandidateSet candidates{};
        bool validationWasRequested = false;
        if (!ReadCandidatesUnlocked(
                component,
                candidates,
                forceRefresh,
                validationWasRequested,
                true,
                decryptIndexOffset)) {
            FinishFailedReadUnlocked(validationWasRequested);
            return false;
        }
        if (!candidates.resolvedValid) {
            SetError(CoordinatePoolRuntimeError::PositionNotFinite);
            FinishFailedReadUnlocked(validationWasRequested);
            return false;
        }
        position = candidates.resolvedPosition;
        FinishReadUnlocked(validationWasRequested);
        return true;
    }

    bool ReadCandidates(std::uintptr_t component,
                        CoordinatePoolCandidateSet& candidates,
                        bool forceRefresh = false) {
        std::lock_guard<std::mutex> lock(mutex);
        candidates = {};
        bool validationWasRequested = false;
        if (!ReadCandidatesUnlocked(
                component,
                candidates,
                forceRefresh,
                validationWasRequested,
                false,
                0)) {
            FinishFailedReadUnlocked(validationWasRequested);
            return false;
        }
        FinishReadUnlocked(validationWasRequested);
        return true;
    }

    bool ReadCandidates(std::uintptr_t component,
                        std::uint32_t decryptIndexOffset,
                        CoordinatePoolCandidateSet& candidates,
                        bool forceRefresh = false) {
        std::lock_guard<std::mutex> lock(mutex);
        candidates = {};
        bool validationWasRequested = false;
        if (!ReadCandidatesUnlocked(
                component,
                candidates,
                forceRefresh,
                validationWasRequested,
                true,
                decryptIndexOffset)) {
            FinishFailedReadUnlocked(validationWasRequested);
            return false;
        }
        FinishReadUnlocked(validationWasRequested);
        return true;
    }

    bool ReadCandidatesUnlocked(
        std::uintptr_t component,
        CoordinatePoolCandidateSet& candidates,
        bool forceRefresh,
        bool& validationWasRequested,
        bool indexed,
        std::uint32_t decryptIndexOffset) {
        ClearReadDiagnosticUnlocked();
        candidates = {};
        ++probe.attempts;
        validationWasRequested = codeValidationRequested;
        if (memory == nullptr || finder == nullptr || engine == nullptr ||
            !executionContext.IsUsable() || !IsValidGuestAddress(component) ||
            indexed != indexedPointers ||
            (indexed &&
             !IsCoordinatePoolDecryptIndexOffsetValid(decryptIndexOffset))) {
            SetError(CoordinatePoolRuntimeError::InvalidInput);
            return false;
        }
        codeValidationRequested = true;
        // Parameters are global to the decoded entry.  Rebuilding them for
        // every component/frame changes the captured key material while the
        // game is publishing its ring and produces alternating coordinates.
        if (!parametersReady && !PrepareParametersUnlocked(component)) {
            return false;
        }
        RefreshPoolPointerUnlocked();

        RingSlot* selected = nullptr;
        auto found = ringSlots.find(component);
        const bool hasSlot = found != ringSlots.end();
        const bool hasRing = found != ringSlots.end() &&
            found->second.ring != 0;
        const bool searchMissing = ShouldSearchCoordinatePoolRing(
            hasSlot,
            hasRing,
            hasSlot ? found->second.stamp : 0,
            frame);
        if (searchMissing &&
            ringSearchBudget.TryConsume(frame)) {
            std::uint64_t ring = 0;
            CoordinatePoolRuntimeError ringError =
                CoordinatePoolRuntimeError::RingSearchFailed;
            if (ExecuteRingSearchUnlocked(component, ring, ringError)) {
                RingSlot& slot = ringSlots[component];
                const std::uint64_t previousRing = slot.ring;
                if (slot.ring != ring) {
                    slot.recovery.Reset();
                    slot.candidateTraceFrame =
                        std::numeric_limits<std::uint64_t>::max();
                    slot.candidateTracePhysicalSlot = UINT8_MAX;
                    slot.previousPositions = {};
                    slot.previousValid = {};
                    slot.hasPreviousPositions = false;
                    slot.indexedPositions = {};
                    slot.indexedBlockCount = 0;
                    slot.indexedResolvedOffset =
                        kCoordinatePoolUnknownDecryptIndexOffset;
                    slot.hasIndexedPositions = false;
                    stableIndexedPositions[component].Reset();
                }
                slot.ring = ring;
                slot.stamp = frame;
                selected = &slot;
                if (IsCoordinatePoolTraceEnabled()) {
                    std::fprintf(
                        stderr,
                        "[coordinate-pool-ring-event] frame=%llu "
                        "component=%llx event=search_success "
                        "previous=%llx ring=%llx failures=%u\n",
                        static_cast<unsigned long long>(frame),
                        static_cast<unsigned long long>(component),
                        static_cast<unsigned long long>(previousRing),
                        static_cast<unsigned long long>(ring),
                        static_cast<unsigned int>(slot.recovery.Failures()));
                }
            } else if (hasRing) {
                // A transient search failure must not publish a different
                // source or erase the last known-good ring.
                selected = &found->second;
            } else {
                RingSlot& slot = ringSlots[component];
                slot.ring = 0;
                slot.stamp = frame;
                if (IsCoordinatePoolTraceEnabled()) {
                    std::fprintf(
                        stderr,
                        "[coordinate-pool-ring-event] frame=%llu "
                        "component=%llx event=search_failed error=%u "
                        "sys=%d read_stage=%u read_failure=%u\n",
                        static_cast<unsigned long long>(frame),
                        static_cast<unsigned long long>(component),
                        static_cast<unsigned int>(ringError),
                        probe.systemError,
                        static_cast<unsigned int>(probe.read.stage),
                        static_cast<unsigned int>(probe.read.failure));
                }
                SetError(ringError);
                return false;
            }
        } else if (hasRing) {
            selected = &found->second;
        } else {
            SetError(CoordinatePoolRuntimeError::RingSearchFailed);
            return false;
        }

        if (indexed) {
            return ReadIndexedCandidateUnlocked(
                component,
                *selected,
                decryptIndexOffset,
                candidates,
                forceRefresh);
        }

        std::uint64_t rawIndexAddress = 0;
        if (!AddSignedOffset(
                selected->ring, finder->index_offset, rawIndexAddress)) {
            SetError(
                CoordinatePoolRuntimeError::PositionReadFailed,
                true,
                -ERANGE);
            return false;
        }
        const std::uint64_t indexAddress =
            NormalizePointer(rawIndexAddress);
        if (!IsCoordinatePoolReadRangeValid(
                indexAddress, sizeof(std::uint64_t))) {
            SetError(
                CoordinatePoolRuntimeError::PositionReadFailed,
                true,
                -ERANGE);
            return false;
        }

        std::uint64_t index = 0;
        std::uint64_t indexAfter = 0;
        std::uint64_t decodedPoolSlot = 0;
        std::uint64_t poolSlot = kCoordinatePoolPhysicalSlotCount;
        std::uint64_t snapshotAddress = 0;
        std::uint64_t coordinateAddress = 0;
        CoordinatePoolPosition selectedCandidate{};
        CoordinatePoolSlotLayout slotLayout = slotLayoutCalibration.Layout();
        if (slotLayout.kind == CoordinatePoolSlotLayoutKind::Conflict) {
            SetError(
                CoordinatePoolRuntimeError::SlotLayoutConflict,
                false,
                -EPROTO);
            return false;
        }
        const bool capturePhysicalSlots =
            IsCoordinatePoolCandidateTraceFullEnabled() ||
            !slotLayout.IsLocked();
        std::size_t capturedPhysicalSlotCount = capturePhysicalSlots
            ? (slotLayout.IsLocked()
                ? slotLayout.physicalSlotCount
                : kCoordinatePoolPhysicalSlotCount)
            : 0;
        bool useBatchSnapshot = slotLayout.IsLocked();
        const auto snapshotSizeFor = [&](std::size_t slotCount) {
            return
                (slotCount - 1) *
                    static_cast<std::size_t>(layout.entryStride) +
                sizeof(CoordinatePoolPosition);
        };
        std::size_t snapshotSize =
            (capturePhysicalSlots
                ? snapshotSizeFor(capturedPhysicalSlotCount)
                : sizeof(CoordinatePoolPosition));
        const std::size_t batchSnapshotSize = slotLayout.IsLocked()
            ? snapshotSizeFor(slotLayout.physicalSlotCount)
            : 0;
        std::vector<std::uint8_t>& snapshot = poolSnapshotScratch;
        if (capturePhysicalSlots || useBatchSnapshot) {
            snapshot.resize(std::max(snapshotSize, batchSnapshotSize));
        }
        std::uint64_t relativeAddress = 0;
        std::uint64_t rawSnapshotAddress = 0;
        if (!AddUnsignedOffset(
                finder->get_ring_offset(),
                layout.poolHeadSkip,
                relativeAddress) ||
            !AddUnsignedOffset(
                selected->ring,
                relativeAddress,
                rawSnapshotAddress)) {
            SetError(
                CoordinatePoolRuntimeError::PositionReadFailed,
                true,
                -ERANGE);
            return false;
        }
        snapshotAddress = NormalizePointer(rawSnapshotAddress);
        if (!IsRemoteAddress(snapshotAddress)) {
            SetError(
                CoordinatePoolRuntimeError::PositionReadFailed,
                true,
                -ERANGE);
            return false;
        }
        bool stable = false;
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (useBatchSnapshot) {
                const bool rangeValid =
                    batchSnapshotSize != 0 &&
                    IsCoordinatePoolReadRangeValid(
                        snapshotAddress, batchSnapshotSize);
                const std::array<MemoryReadRequest, 3> requests{{
                    {indexAddress, &index, sizeof(index)},
                    {snapshotAddress, snapshot.data(), batchSnapshotSize},
                    {indexAddress, &indexAfter, sizeof(indexAfter)},
                }};
                const std::array<CoordinateReadStage, 3> stages{{
                    CoordinateReadStage::RingIndex,
                    CoordinateReadStage::Position,
                    CoordinateReadStage::RingIndex,
                }};
                std::size_t failedIndex = requests.size();
                if (rangeValid && ReadRemoteBatchUnlocked(
                        requests.data(),
                        requests.size(),
                        stages.data(),
                        failedIndex)) {
                    decodedPoolSlot = finder->decode_ring_slot(index);
                    poolSlot = MapDecodedCoordinatePoolSlot(
                        decodedPoolSlot, slotLayout);
                    if (index == indexAfter) {
                        if (poolSlot < kCoordinatePoolPhysicalSlotCount) {
                            const std::size_t slotOffset =
                                poolSlot *
                                static_cast<std::size_t>(layout.entryStride);
                            std::memcpy(
                                &selectedCandidate,
                                snapshot.data() + slotOffset,
                                sizeof(selectedCandidate));
                            if (!AddUnsignedOffset(
                                    snapshotAddress,
                                    slotOffset,
                                    coordinateAddress)) {
                                SetError(
                                    CoordinatePoolRuntimeError::PositionReadFailed,
                                    true,
                                    -ERANGE);
                                return false;
                            }
                        }
                        stable = true;
                        break;
                    }
                    continue;
                }
                ClearReadDiagnosticUnlocked();
                useBatchSnapshot = false;
            }
            if (!ReadRemoteUnlocked(
                    indexAddress,
                    &index,
                    sizeof(index),
                    CoordinateReadStage::RingIndex)) {
                break;
            }
            decodedPoolSlot = finder->decode_ring_slot(index);
            poolSlot = kCoordinatePoolPhysicalSlotCount;
            if (slotLayout.IsLocked()) {
                poolSlot = MapDecodedCoordinatePoolSlot(
                    decodedPoolSlot, slotLayout);
                if (poolSlot >= kCoordinatePoolPhysicalSlotCount) {
                    if (!ReadRemoteUnlocked(
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
                    continue;
                }
            }
            if (capturePhysicalSlots) {
                bool snapshotRead =
                    IsCoordinatePoolReadRangeValid(
                        snapshotAddress, snapshotSize) &&
                    ReadRemoteUnlocked(
                        snapshotAddress,
                        snapshot.data(),
                        snapshotSize,
                        CoordinateReadStage::Position);
                if (ShouldRetryCoordinatePoolCompactSnapshot(
                        slotLayout,
                        slotLayoutCalibration.CompactPossible(),
                        decodedPoolSlot,
                        capturedPhysicalSlotCount,
                        snapshotRead)) {
                    capturedPhysicalSlotCount =
                        kCoordinatePoolCompactLayout.physicalSlotCount;
                    snapshotSize = snapshotSizeFor(
                        capturedPhysicalSlotCount);
                    snapshot.resize(snapshotSize);
                    snapshotRead = IsCoordinatePoolReadRangeValid(
                            snapshotAddress, snapshotSize) &&
                        ReadRemoteUnlocked(
                            snapshotAddress,
                            snapshot.data(),
                            snapshotSize,
                            CoordinateReadStage::Position);
                    if (snapshotRead) ClearReadDiagnosticUnlocked();
                }
                if (!snapshotRead && !probe.read.HasFailure()) {
                    SetError(
                        CoordinatePoolRuntimeError::PositionReadFailed,
                        true,
                        -ERANGE);
                    return false;
                }
                if (!snapshotRead) break;
            } else {
                if (!slotLayout.IsLocked()) {
                    SetError(
                        CoordinatePoolRuntimeError::SlotLayoutPending,
                        false,
                        -EAGAIN);
                    return false;
                }
                const std::uint64_t slotOffset =
                    poolSlot * static_cast<std::uint64_t>(layout.entryStride);
                if (!AddUnsignedOffset(
                        snapshotAddress, slotOffset, coordinateAddress) ||
                    !IsCoordinatePoolReadRangeValid(
                        coordinateAddress,
                        sizeof(CoordinatePoolPosition))) {
                    SetError(
                        CoordinatePoolRuntimeError::PositionReadFailed,
                        true,
                        -ERANGE);
                    return false;
                }
                if (!ReadRemoteUnlocked(
                        coordinateAddress,
                        &selectedCandidate,
                        sizeof(selectedCandidate),
                        CoordinateReadStage::Position)) {
                    break;
                }
            }
            if (!ReadRemoteUnlocked(
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
            const CoordinatePoolRuntimeError error = probe.read.HasFailure()
                ? CoordinatePoolRuntimeError::PositionReadFailed
                : CoordinatePoolRuntimeError::PositionUnstable;
            SetError(error);
            const CoordinatePoolRingReadEvent event =
                IsCoordinatePoolRingRemoteReadFailure(error, probe.read)
                ? CoordinatePoolRingReadEvent::RemoteReadFailure
                : CoordinatePoolRingReadEvent::OtherFailure;
            const bool invalidateRing = selected->recovery.Observe(event);
            if (IsCoordinatePoolTraceEnabled() &&
                event == CoordinatePoolRingReadEvent::RemoteReadFailure) {
                std::fprintf(
                    stderr,
                    "[coordinate-pool-ring-event] frame=%llu "
                    "component=%llx event=remote_read_failure ring=%llx "
                    "failures=%u invalidate=%d sys=%d read_stage=%u "
                    "read_failure=%u read_path=%u read_at=%llx "
                    "read_n=%zu\n",
                    static_cast<unsigned long long>(frame),
                    static_cast<unsigned long long>(component),
                    static_cast<unsigned long long>(selected->ring),
                    static_cast<unsigned int>(selected->recovery.Failures()),
                    invalidateRing ? 1 : 0,
                    probe.systemError,
                    static_cast<unsigned int>(probe.read.stage),
                    static_cast<unsigned int>(probe.read.failure),
                    static_cast<unsigned int>(probe.read.lastPath),
                    static_cast<unsigned long long>(probe.read.address),
                    probe.read.size);
            }
            if (invalidateRing) {
                ringSlots.erase(component);
            }
            return false;
        }

        const CoordinatePoolSlotLayoutKind previousLayoutKind =
            slotLayout.kind;
        slotLayout = slotLayoutCalibration.ObserveDecodedSlot(
            decodedPoolSlot);
        UpdateSlotLayoutProbeUnlocked();
        if (slotLayout.kind != previousLayoutKind) {
            TraceSlotLayoutUnlocked(
                "decoded",
                component,
                decodedPoolSlot,
                0);
        }
        if (slotLayout.kind == CoordinatePoolSlotLayoutKind::Conflict) {
            SetError(
                CoordinatePoolRuntimeError::SlotLayoutConflict,
                false,
                -EPROTO);
            return false;
        }
        if (slotLayout.IsLocked()) {
            poolSlot = MapDecodedCoordinatePoolSlot(
                decodedPoolSlot, slotLayout);
            if (poolSlot >= kCoordinatePoolPhysicalSlotCount) {
                SetError(
                    CoordinatePoolRuntimeError::SlotLayoutConflict,
                    false,
                    -ERANGE);
                return false;
            }
        }

        candidates.ring = selected->ring;
        candidates.index = index;
        candidates.decodedPhysicalSlot =
            static_cast<std::uint8_t>(decodedPoolSlot);
        candidates.decodedSlotMask = slotLayoutCalibration.DecodedMask();
        if (!capturePhysicalSlots) {
            candidates.logicalSlotCount = slotLayout.logicalSlotCount;
            candidates.physicalSlotCount = slotLayout.physicalSlotCount;
            candidates.slotPhase = slotLayout.phase;
            candidates.selectedPhysicalSlot =
                static_cast<std::uint8_t>(poolSlot);
            candidates.activeBank = static_cast<std::uint8_t>(
                poolSlot / slotLayout.logicalSlotCount);
            candidates.selectedLogicalSlot = static_cast<std::uint8_t>(
                poolSlot % slotLayout.logicalSlotCount);
            const std::size_t selectedLogical =
                candidates.selectedLogicalSlot;
            const bool selectedValid = IsFinitePosition(selectedCandidate);
            candidates.positions[selectedLogical] = selectedCandidate;
            candidates.valid[selectedLogical] = selectedValid;
            candidates.physicalPositions[poolSlot] = selectedCandidate;
            candidates.physicalValid[poolSlot] = selectedValid;
            if (!IsCoordinatePoolSelectedCandidateValid(candidates)) {
                selected->recovery.Observe(
                    CoordinatePoolRingReadEvent::OtherFailure);
                SetError(CoordinatePoolRuntimeError::PositionNotFinite);
                return false;
            }
            selected->recovery.Observe(
                CoordinatePoolRingReadEvent::Success);
            if (IsCoordinatePoolTraceEnabled()) {
                std::fprintf(
                    stderr,
                    "[coordinate-pool-trace] frame=%llu component=%llx "
                    "parameter_component=%llx parameter_fingerprint=%llx "
                    "ring_offset=%u index_offset=%lld entry_stride=%u "
                    "pool_head_skip=%u pool_pointer_offset=%d "
                    "force=%d pool=%llx ring=%llx index=%llx "
                    "decoded_slot=%llu slot=%llu "
                    "address=%llx xyz=(%.3f,%.3f,%.3f) fast=1\n",
                    static_cast<unsigned long long>(frame),
                    static_cast<unsigned long long>(component),
                    static_cast<unsigned long long>(parameterComponent),
                    static_cast<unsigned long long>(parameterFingerprint),
                    static_cast<unsigned int>(finder->get_ring_offset()),
                    static_cast<long long>(finder->index_offset),
                    static_cast<unsigned int>(layout.entryStride),
                    static_cast<unsigned int>(layout.poolHeadSkip),
                    static_cast<int>(finder->pool_ptr_offset),
                    forceRefresh ? 1 : 0,
                    static_cast<unsigned long long>(lastPoolPointer),
                    static_cast<unsigned long long>(selected->ring),
                    static_cast<unsigned long long>(index),
                    static_cast<unsigned long long>(decodedPoolSlot),
                    static_cast<unsigned long long>(poolSlot),
                    static_cast<unsigned long long>(coordinateAddress),
                    selectedCandidate.x,
                    selectedCandidate.y,
                    selectedCandidate.z);
            }
            return true;
        }
        for (std::size_t physicalSlot = 0;
             physicalSlot < capturedPhysicalSlotCount;
             ++physicalSlot) {
            const std::size_t offset =
                physicalSlot * static_cast<std::size_t>(layout.entryStride);
            CoordinatePoolPosition candidate{};
            std::memcpy(
                &candidate,
                snapshot.data() + offset,
                sizeof(candidate));
            candidates.physicalPositions[physicalSlot] = candidate;
            candidates.physicalValid[physicalSlot] =
                IsFinitePosition(candidate);
            if (selected->hasPreviousPositions &&
                selected->previousPhysicalSlotCount ==
                    capturedPhysicalSlotCount &&
                (selected->previousValid[physicalSlot] !=
                     candidates.physicalValid[physicalSlot] ||
                 std::memcmp(
                     &selected->previousPositions[physicalSlot],
                     &candidate,
                     sizeof(candidate)) != 0)) {
                candidates.changedPhysicalMask |=
                    static_cast<std::uint16_t>(1U << physicalSlot);
                ++candidates.changedPhysicalCount;
                candidates.newestPhysicalSlot =
                    static_cast<std::uint8_t>(physicalSlot);
            }
        }
        if (candidates.changedPhysicalCount != 1) {
            candidates.newestPhysicalSlot = UINT8_MAX;
        }

        const CoordinatePoolSlotLayoutKind previousTransitionLayoutKind =
            slotLayout.kind;
        if (selected->hasPreviousPositions &&
            selected->previousPhysicalSlotCount ==
                capturedPhysicalSlotCount) {
            const std::size_t evidenceBefore =
                slotLayoutCalibration.EvidenceCount(
                    CoordinatePoolSlotLayoutKind::Compact) +
                slotLayoutCalibration.EvidenceCount(
                    CoordinatePoolSlotLayoutKind::Extended);
            const std::uint16_t compactMaskBefore =
                slotLayoutCalibration.CompactPhaseMask();
            const std::uint16_t extendedMaskBefore =
                slotLayoutCalibration.ExtendedPhaseMask();
            slotLayout = slotLayoutCalibration.ObserveTransition(
                component,
                selected->previousIndex,
                index,
                selected->previousDecodedSlot,
                decodedPoolSlot,
                candidates.changedPhysicalMask);
            UpdateSlotLayoutProbeUnlocked();
            const std::size_t evidenceAfter =
                slotLayoutCalibration.EvidenceCount(
                    CoordinatePoolSlotLayoutKind::Compact) +
                slotLayoutCalibration.EvidenceCount(
                    CoordinatePoolSlotLayoutKind::Extended);
            if (slotLayout.kind != previousTransitionLayoutKind ||
                evidenceAfter != evidenceBefore ||
                slotLayoutCalibration.CompactPhaseMask() !=
                    compactMaskBefore ||
                slotLayoutCalibration.ExtendedPhaseMask() !=
                    extendedMaskBefore) {
                TraceSlotLayoutUnlocked(
                    "transition",
                    component,
                    decodedPoolSlot,
                    candidates.changedPhysicalMask);
            }
        }
        selected->previousPositions = candidates.physicalPositions;
        selected->previousValid = candidates.physicalValid;
        selected->previousIndex = index;
        selected->previousDecodedSlot = decodedPoolSlot;
        selected->previousPhysicalSlotCount =
            static_cast<std::uint8_t>(capturedPhysicalSlotCount);
        selected->hasPreviousPositions = true;

        if (slotLayout.kind == CoordinatePoolSlotLayoutKind::Conflict) {
            SetError(
                CoordinatePoolRuntimeError::SlotLayoutConflict,
                false,
                -EPROTO);
            return false;
        }
        if (!slotLayout.IsLocked()) {
            const bool hasLayoutEvidence =
                slotLayoutCalibration.EvidenceCount(
                    CoordinatePoolSlotLayoutKind::Compact) != 0 ||
                slotLayoutCalibration.EvidenceCount(
                    CoordinatePoolSlotLayoutKind::Extended) != 0;
            SetError(
                hasLayoutEvidence
                    ? CoordinatePoolRuntimeError::SlotLayoutPending
                    : CoordinatePoolRuntimeError::SlotLayoutEvidenceMissing,
                false,
                -EAGAIN);
            return false;
        }
        poolSlot = MapDecodedCoordinatePoolSlot(decodedPoolSlot, slotLayout);
        if (poolSlot >= slotLayout.physicalSlotCount) {
            SetError(
                CoordinatePoolRuntimeError::SlotLayoutConflict,
                false,
                -ERANGE);
            return false;
        }
        candidates.logicalSlotCount = slotLayout.logicalSlotCount;
        candidates.physicalSlotCount = slotLayout.physicalSlotCount;
        candidates.slotPhase = slotLayout.phase;
        candidates.selectedPhysicalSlot =
            static_cast<std::uint8_t>(poolSlot);
        candidates.activeBank = static_cast<std::uint8_t>(
            poolSlot / slotLayout.logicalSlotCount);
        candidates.selectedLogicalSlot = static_cast<std::uint8_t>(
            poolSlot % slotLayout.logicalSlotCount);

        const std::size_t bankFirstSlot =
            static_cast<std::size_t>(candidates.activeBank) *
            slotLayout.logicalSlotCount;
        std::uint32_t validMask = 0;
        for (std::size_t logicalSlot = 0;
             logicalSlot < slotLayout.logicalSlotCount;
             ++logicalSlot) {
            const CoordinatePoolPosition candidate =
                candidates.physicalPositions[bankFirstSlot + logicalSlot];
            candidates.positions[logicalSlot] = candidate;
            candidates.valid[logicalSlot] =
                candidates.physicalValid[bankFirstSlot + logicalSlot];
            if (candidates.valid[logicalSlot]) {
                validMask |= 1U << logicalSlot;
            }
        }
        if (!IsCoordinatePoolSelectedCandidateValid(candidates)) {
            selected->recovery.Observe(
                CoordinatePoolRingReadEvent::OtherFailure);
            SetError(CoordinatePoolRuntimeError::PositionNotFinite);
            return false;
        }
        selected->recovery.Observe(CoordinatePoolRingReadEvent::Success);

        const std::size_t selectedLogical =
            candidates.selectedLogicalSlot;
        const CoordinatePoolPosition& candidate =
            candidates.positions[selectedLogical];
        coordinateAddress = snapshotAddress +
            poolSlot * static_cast<std::uint64_t>(layout.entryStride);
        if (capturePhysicalSlots && IsCoordinatePoolTraceEnabled()) {
            std::fprintf(
                stderr,
                "[coordinate-pool-trace] frame=%llu component=%llx "
                "parameter_component=%llx parameter_fingerprint=%llx "
                "ring_offset=%u index_offset=%lld entry_stride=%u "
                "pool_head_skip=%u pool_pointer_offset=%d "
                "force=%d pool=%llx ring=%llx index=%llx "
                "decoded_slot=%llu slot=%llu "
                "address=%llx xyz=(%.3f,%.3f,%.3f)\n",
                static_cast<unsigned long long>(frame),
                static_cast<unsigned long long>(component),
                static_cast<unsigned long long>(parameterComponent),
                static_cast<unsigned long long>(parameterFingerprint),
                static_cast<unsigned int>(finder->get_ring_offset()),
                static_cast<long long>(finder->index_offset),
                static_cast<unsigned int>(layout.entryStride),
                static_cast<unsigned int>(layout.poolHeadSkip),
                static_cast<int>(finder->pool_ptr_offset),
                forceRefresh ? 1 : 0,
                static_cast<unsigned long long>(lastPoolPointer),
                static_cast<unsigned long long>(selected->ring),
                static_cast<unsigned long long>(index),
                static_cast<unsigned long long>(decodedPoolSlot),
                static_cast<unsigned long long>(poolSlot),
                static_cast<unsigned long long>(coordinateAddress),
                candidate.x,
                candidate.y,
                candidate.z);

            const bool selectedChanged =
                (candidates.changedPhysicalMask &
                 (static_cast<std::uint16_t>(1U) << poolSlot)) != 0;
            CoordinatePoolPosition newest{};
            if (candidates.newestPhysicalSlot <
                candidates.physicalPositions.size()) {
                newest = candidates.physicalPositions[
                    candidates.newestPhysicalSlot];
            }
            std::fprintf(
                stderr,
                "[coordinate-pool-write] frame=%llu component=%llx "
                "ring=%llx index=%llx decoded=%u selected=%u "
                "changed_mask=%04x "
                "changed_count=%u newest=%u selected_changed=%d "
                "newest_xyz=(%.3f,%.3f,%.3f)\n",
                static_cast<unsigned long long>(frame),
                static_cast<unsigned long long>(component),
                static_cast<unsigned long long>(selected->ring),
                static_cast<unsigned long long>(index),
                static_cast<unsigned int>(
                    candidates.decodedPhysicalSlot),
                static_cast<unsigned int>(
                    candidates.selectedPhysicalSlot),
                static_cast<unsigned int>(
                    candidates.changedPhysicalMask),
                static_cast<unsigned int>(
                    candidates.changedPhysicalCount),
                static_cast<unsigned int>(
                    candidates.newestPhysicalSlot),
                selectedChanged ? 1 : 0,
                newest.x,
                newest.y,
                newest.z);

            const bool neverTraced = selected->candidateTraceFrame ==
                std::numeric_limits<std::uint64_t>::max();
            const std::uint64_t elapsed = neverTraced ||
                    frame < selected->candidateTraceFrame
                ? std::numeric_limits<std::uint64_t>::max()
                : frame - selected->candidateTraceFrame;
            const bool physicalSlotChanged =
                selected->candidateTracePhysicalSlot != poolSlot;
            if (IsCoordinatePoolCandidateTraceFullEnabled() || neverTraced ||
                elapsed >= kCandidateTracePeriodicInterval ||
                (physicalSlotChanged &&
                 elapsed >= kCandidateTraceMinimumInterval)) {
                std::fprintf(
                    stderr,
                    "[coordinate-pool-candidates] frame=%llu "
                    "component=%llx ring=%llx index=%llx decoded=%u "
                    "physical=%u "
                    "bank=%u selected=%u valid_mask=%02x "
                    "c0=(%.3f,%.3f,%.3f) c1=(%.3f,%.3f,%.3f) "
                    "c2=(%.3f,%.3f,%.3f) c3=(%.3f,%.3f,%.3f) "
                    "c4=(%.3f,%.3f,%.3f) c5=(%.3f,%.3f,%.3f) "
                    "c6=(%.3f,%.3f,%.3f)\n",
                    static_cast<unsigned long long>(frame),
                    static_cast<unsigned long long>(component),
                    static_cast<unsigned long long>(selected->ring),
                    static_cast<unsigned long long>(index),
                    static_cast<unsigned int>(
                        candidates.decodedPhysicalSlot),
                    static_cast<unsigned int>(
                        candidates.selectedPhysicalSlot),
                    static_cast<unsigned int>(candidates.activeBank),
                    static_cast<unsigned int>(
                        candidates.selectedLogicalSlot),
                    static_cast<unsigned int>(validMask),
                    candidates.positions[0].x,
                    candidates.positions[0].y,
                    candidates.positions[0].z,
                    candidates.positions[1].x,
                    candidates.positions[1].y,
                    candidates.positions[1].z,
                    candidates.positions[2].x,
                    candidates.positions[2].y,
                    candidates.positions[2].z,
                    candidates.positions[3].x,
                    candidates.positions[3].y,
                    candidates.positions[3].z,
                    candidates.positions[4].x,
                    candidates.positions[4].y,
                    candidates.positions[4].z,
                    candidates.positions[5].x,
                    candidates.positions[5].y,
                    candidates.positions[5].z,
                    candidates.positions[6].x,
                    candidates.positions[6].y,
                    candidates.positions[6].z);
                selected->candidateTraceFrame = frame;
                selected->candidateTracePhysicalSlot =
                    candidates.selectedPhysicalSlot;
            }
        }

        return true;
    }

    bool TryConsumeDecryptIndexCalibrationReadUnlocked(
        std::uint64_t component) noexcept {
        if (decryptIndexCalibration.IsLocked() &&
            decryptIndexCalibrationWitnessCount != 0) {
            decryptIndexCalibrationWitnessCount =
                ExpireCoordinatePoolDecryptIndexWitnesses(
                    decryptIndexCalibrationWitnesses,
                    decryptIndexCalibrationWitnessLastSeenFrames,
                    decryptIndexCalibrationWitnessCount,
                    frame);
        }
        if (decryptIndexCalibrationFrame != frame) {
            const std::size_t previousVisitCount =
                decryptIndexCalibrationVisitCount;
            decryptIndexCalibrationFrame = frame;
            decryptIndexCalibrationReads = 0;
            decryptIndexCalibrationPreviousVisitCount =
                previousVisitCount;
            decryptIndexCalibrationVisitCount = 0;
            const std::size_t evidence =
                decryptIndexCalibration.Evidence();
            if (decryptIndexCalibrationProgressFrame ==
                    std::numeric_limits<std::uint64_t>::max() ||
                evidence != decryptIndexCalibrationLastEvidence) {
                decryptIndexCalibrationProgressFrame = frame;
                decryptIndexCalibrationLastEvidence = evidence;
            }
            if (!decryptIndexCalibration.IsLocked() &&
                decryptIndexCalibrationWitnessCount != 0 &&
                frame >= decryptIndexCalibrationProgressFrame &&
                frame - decryptIndexCalibrationProgressFrame >=
                    kCoordinatePoolDecryptIndexWitnessRefreshFrames) {
                if (previousVisitCount != 0) {
                    decryptIndexCalibrationWindowStart =
                        (decryptIndexCalibrationWindowStart +
                         kCoordinatePoolDecryptIndexCalibrationReadsPerFrame) %
                        previousVisitCount;
                }
                decryptIndexCalibrationWitnesses = {};
                decryptIndexCalibrationWitnessLastSeenFrames = {};
                decryptIndexCalibrationWitnessCount = 0;
                decryptIndexCalibrationProgressFrame = frame;
            }
        }
        const std::size_t ordinal =
            decryptIndexCalibrationVisitCount++;
        const std::size_t readLimit =
            decryptIndexCalibration.IsLocked()
            ? kCoordinatePoolDecryptIndexAuditReadsPerFrame
            : kCoordinatePoolDecryptIndexCalibrationReadsPerFrame;

        std::size_t witnessIndex =
            decryptIndexCalibrationWitnessCount;
        for (std::size_t index = 0;
             index < decryptIndexCalibrationWitnessCount;
             ++index) {
            if (decryptIndexCalibrationWitnesses[index] == component) {
                witnessIndex = index;
                break;
            }
        }
        if (witnessIndex < decryptIndexCalibrationWitnessCount) {
            decryptIndexCalibrationWitnessLastSeenFrames[witnessIndex] =
                frame;
            if (decryptIndexCalibrationReads >= readLimit) return false;
        } else if (decryptIndexCalibrationReads >= readLimit) {
            return false;
        }
        if (witnessIndex == decryptIndexCalibrationWitnessCount) {
            const std::size_t witnessLimit =
                decryptIndexCalibration.IsLocked()
                ? std::min<std::size_t>(
                      decryptIndexCalibrationWitnesses.size(),
                      kCoordinatePoolDecryptIndexAuditReadsPerFrame)
                : decryptIndexCalibrationWitnesses.size();
            if (decryptIndexCalibrationWitnessCount >= witnessLimit) {
                return false;
            }
            const std::size_t population =
                decryptIndexCalibrationPreviousVisitCount;
            bool selected = decryptIndexCalibration.IsLocked();
            if (!selected) {
                selected = population == 0
                    ? ordinal <
                        kCoordinatePoolDecryptIndexCalibrationReadsPerFrame
                    : kCoordinatePoolDecryptIndexCalibrationReadsPerFrame >=
                        population;
                if (!selected && population != 0 && ordinal < population) {
                    const std::size_t distance =
                        (ordinal + population -
                         decryptIndexCalibrationWindowStart) %
                        population;
                    selected = distance <
                        kCoordinatePoolDecryptIndexCalibrationReadsPerFrame;
                }
            }
            if (!selected) return false;
            witnessIndex = decryptIndexCalibrationWitnessCount++;
            decryptIndexCalibrationWitnesses[witnessIndex] = component;
            decryptIndexCalibrationWitnessLastSeenFrames[witnessIndex] =
                frame;
        }
        ++decryptIndexCalibrationReads;
        return true;
    }

    void ResetDecryptIndexWitnessesUnlocked() noexcept {
        decryptIndexCalibrationFrame =
            std::numeric_limits<std::uint64_t>::max();
        decryptIndexCalibrationReads = 0;
        decryptIndexCalibrationVisitCount = 0;
        decryptIndexCalibrationPreviousVisitCount = 0;
        decryptIndexCalibrationWindowStart = 0;
        decryptIndexCalibrationWitnesses = {};
        decryptIndexCalibrationWitnessLastSeenFrames = {};
        decryptIndexCalibrationWitnessCount = 0;
        decryptIndexCalibrationProgressFrame = frame;
        decryptIndexCalibrationLastEvidence =
            decryptIndexCalibration.Evidence();
    }

    void UpdateDecryptIndexEffectiveOffsetUnlocked() {
        if (pendingDecryptIndexOffset ==
                kCoordinatePoolUnknownDecryptIndexOffset ||
            pendingDecryptIndexFrame == frame) {
            return;
        }
        effectiveDecryptIndexOffset = pendingDecryptIndexOffset;
        pendingDecryptIndexOffset =
            kCoordinatePoolUnknownDecryptIndexOffset;
        stableIndexedPositions.clear();
    }

    void ScheduleDecryptIndexOffsetUnlocked() noexcept {
        if (!decryptIndexCalibration.IsLocked()) return;
        const std::uint8_t selected = decryptIndexCalibration.Selected();
        if (selected == effectiveDecryptIndexOffset ||
            selected == pendingDecryptIndexOffset) {
            return;
        }
        pendingDecryptIndexOffset = selected;
        pendingDecryptIndexFrame = frame;
    }

    bool ReadIndexedCandidateUnlocked(
        std::uintptr_t component,
        RingSlot& selected,
        std::uint32_t decryptIndexOffset,
        CoordinatePoolCandidateSet& candidates,
        bool forceRefresh) {
        std::uint64_t rawIndexAddress = 0;
        if (!AddSignedOffset(
                selected.ring, finder->index_offset, rawIndexAddress)) {
            SetError(
                CoordinatePoolRuntimeError::PositionReadFailed,
                true,
                -ERANGE);
            return false;
        }
        const std::uint64_t indexAddress =
            NormalizePointer(rawIndexAddress);
        if (!IsCoordinatePoolReadRangeValid(
                indexAddress, sizeof(std::uint64_t))) {
            SetError(
                CoordinatePoolRuntimeError::PositionReadFailed,
                true,
                -ERANGE);
            return false;
        }

        std::uint64_t relativeAddress = 0;
        std::uint64_t rawSnapshotAddress = 0;
        if (!AddUnsignedOffset(
                finder->get_ring_offset(),
                layout.poolHeadSkip,
                relativeAddress) ||
            !AddUnsignedOffset(
                selected.ring,
                relativeAddress,
                rawSnapshotAddress)) {
            SetError(
                CoordinatePoolRuntimeError::PositionReadFailed,
                true,
                -ERANGE);
            return false;
        }
        const std::uint64_t snapshotAddress =
            NormalizePointer(rawSnapshotAddress);
        if (!IsRemoteAddress(snapshotAddress)) {
            SetError(
                CoordinatePoolRuntimeError::PositionReadFailed,
                true,
                -ERANGE);
            return false;
        }

        if (forceRefresh) {
            predictedPoolBlockCount = 0;
            stableIndexedPositions[component].Reset();
            selected.indexedPositions = {};
            selected.indexedBlockCount = 0;
            selected.indexedResolvedOffset =
                kCoordinatePoolUnknownDecryptIndexOffset;
            selected.hasIndexedPositions = false;
        }
        UpdateDecryptIndexEffectiveOffsetUnlocked();

        std::uint64_t index = 0;
        if (!ReadRemoteUnlocked(
                indexAddress,
                &index,
                sizeof(index),
                CoordinateReadStage::RingIndex)) {
            return FinishIndexedRemoteReadFailureUnlocked();
        }
        std::uint64_t decodedPoolSlot =
            finder->decode_ring_slot(index);
        CoordinatePoolPosition current{};
        std::uint64_t indexAfter = 0;
        std::uint8_t resolvedDecryptIndexOffset =
            effectiveDecryptIndexOffset !=
                kCoordinatePoolUnknownDecryptIndexOffset
            ? effectiveDecryptIndexOffset
            : static_cast<std::uint8_t>(decryptIndexOffset);
        std::size_t poolSlot = kCoordinatePoolBlockProbeCount;
        bool calibrationSnapshotRead = false;
        std::array<CoordinatePoolPosition,
                   kCoordinatePoolBlockProbeCount> indexedPositions{};
        const bool calibrating =
            TryConsumeDecryptIndexCalibrationReadUnlocked(component);
        if (calibrating || predictedPoolBlockCount == 0) {
            const std::size_t blocksToRead =
                predictedPoolBlockCount == 0
                ? kCoordinatePoolBlockProbeCount
                : predictedPoolBlockCount;
            const std::size_t snapshotSize =
                blocksToRead *
                static_cast<std::size_t>(layout.entryStride);
            if (!IsCoordinatePoolReadRangeValid(
                    snapshotAddress, snapshotSize)) {
                SetError(
                    CoordinatePoolRuntimeError::RingPreparationFailed,
                    false,
                    -ERANGE);
                return false;
            }
            poolSnapshotScratch.resize(snapshotSize);
            if (!ReadRemoteUnlocked(
                    snapshotAddress,
                    poolSnapshotScratch.data(),
                    snapshotSize,
                    CoordinateReadStage::Position)) {
                SetError(
                    predictedPoolBlockCount == 0
                        ? CoordinatePoolRuntimeError::RingPreparationFailed
                        : CoordinatePoolRuntimeError::PositionReadFailed);
                return false;
            }
            for (std::size_t block = 0; block < blocksToRead; ++block) {
                std::memcpy(
                    &indexedPositions[block],
                    poolSnapshotScratch.data() +
                        block * static_cast<std::size_t>(layout.entryStride),
                    sizeof(indexedPositions[block]));
            }
            if (predictedPoolBlockCount == 0) {
                const std::size_t predicted =
                    PredictCoordinatePoolBlockCount(
                        indexedPositions.data(),
                        indexedPositions.size());
                if (predicted == 0 ||
                    predicted > kCoordinatePoolMaximumBlockCount) {
                    SetError(
                        CoordinatePoolRuntimeError::RingPreparationFailed,
                        false,
                        -ENODATA);
                    return false;
                }
                if (decryptIndexCalibrationBlockCount != 0 &&
                    decryptIndexCalibrationBlockCount != predicted) {
                    decryptIndexCalibration.Reset();
                    effectiveDecryptIndexOffset =
                        kCoordinatePoolUnknownDecryptIndexOffset;
                    pendingDecryptIndexOffset =
                        kCoordinatePoolUnknownDecryptIndexOffset;
                    pendingDecryptIndexFrame =
                        std::numeric_limits<std::uint64_t>::max();
                    stableIndexedPositions.clear();
                    decryptIndexCalibrationWitnesses = {};
                    decryptIndexCalibrationWitnessLastSeenFrames = {};
                    decryptIndexCalibrationWitnessCount = 0;
                    decryptIndexCalibrationProgressFrame = frame;
                    decryptIndexCalibrationLastEvidence = 0;
                }
                predictedPoolBlockCount =
                    static_cast<std::uint8_t>(predicted);
                decryptIndexCalibrationBlockCount =
                    predictedPoolBlockCount;
            }
            if (!ReadRemoteUnlocked(
                    indexAddress,
                    &indexAfter,
                    sizeof(indexAfter),
                    CoordinateReadStage::RingIndex)) {
                return FinishIndexedRemoteReadFailureUnlocked();
            }
            calibrationSnapshotRead = true;
            if (index == indexAfter) {
                std::uint32_t changedSlotMask = 0;
                if (selected.hasIndexedPositions &&
                    selected.indexedBlockCount ==
                        predictedPoolBlockCount &&
                    selected.previousIndex != index &&
                    selected.previousDecodedSlot != decodedPoolSlot) {
                    for (std::size_t block = 0;
                         block < predictedPoolBlockCount;
                         ++block) {
                        if (std::memcmp(
                                &selected.indexedPositions[block],
                                &indexedPositions[block],
                                sizeof(CoordinatePoolPosition)) != 0 &&
                            IsFinitePosition(indexedPositions[block]) &&
                            !IsCoordinatePoolBlockTerminator(
                                indexedPositions[block])) {
                            changedSlotMask |=
                                UINT32_C(1) << block;
                        }
                    }
                }
                if (changedSlotMask != 0) {
                    const std::uint16_t matchingOffsets =
                        MatchingCoordinatePoolDecryptIndexOffsets(
                            decodedPoolSlot,
                            changedSlotMask,
                            predictedPoolBlockCount);
                    decryptIndexCalibration.Observe(
                        component, matchingOffsets);
                    ScheduleDecryptIndexOffsetUnlocked();
                }
                selected.indexedPositions = indexedPositions;
                selected.indexedBlockCount = predictedPoolBlockCount;
                selected.previousIndex = index;
                selected.previousDecodedSlot = decodedPoolSlot;
                selected.hasIndexedPositions = true;
            } else {
                index = indexAfter;
                decodedPoolSlot = finder->decode_ring_slot(index);
                calibrationSnapshotRead = false;
            }
            resolvedDecryptIndexOffset =
                effectiveDecryptIndexOffset !=
                    kCoordinatePoolUnknownDecryptIndexOffset
                ? effectiveDecryptIndexOffset
                : static_cast<std::uint8_t>(decryptIndexOffset);
            poolSlot = SelectCoordinatePoolIndexedSlot(
                decodedPoolSlot,
                resolvedDecryptIndexOffset,
                predictedPoolBlockCount);
            if (poolSlot < predictedPoolBlockCount) {
                current = indexedPositions[poolSlot];
            }
        } else {
            resolvedDecryptIndexOffset =
                effectiveDecryptIndexOffset !=
                    kCoordinatePoolUnknownDecryptIndexOffset
                ? effectiveDecryptIndexOffset
                : static_cast<std::uint8_t>(decryptIndexOffset);
            poolSlot = SelectCoordinatePoolIndexedSlot(
                decodedPoolSlot,
                resolvedDecryptIndexOffset,
                predictedPoolBlockCount);
        }
        if (poolSlot >= predictedPoolBlockCount) {
            SetError(
                CoordinatePoolRuntimeError::PositionReadFailed,
                false,
                -ERANGE);
            return false;
        }
        const std::uint64_t slotOffset =
            poolSlot * static_cast<std::uint64_t>(layout.entryStride);
        std::uint64_t coordinateAddress = 0;
        if (!AddUnsignedOffset(
                snapshotAddress, slotOffset, coordinateAddress) ||
            !IsCoordinatePoolReadRangeValid(
                coordinateAddress,
                sizeof(CoordinatePoolPosition))) {
            SetError(
                CoordinatePoolRuntimeError::PositionReadFailed,
                true,
                -ERANGE);
            return false;
        }
        if (!calibrationSnapshotRead) {
            if (!ReadRemoteUnlocked(
                    coordinateAddress,
                    &current,
                    sizeof(current),
                    CoordinateReadStage::Position) ||
                !ReadRemoteUnlocked(
                    indexAddress,
                    &indexAfter,
                    sizeof(indexAfter),
                    CoordinateReadStage::RingIndex)) {
                return FinishIndexedRemoteReadFailureUnlocked();
            }
        }
        if (selected.indexedResolvedOffset !=
            resolvedDecryptIndexOffset) {
            stableIndexedPositions[component].Reset();
            selected.indexedResolvedOffset =
                resolvedDecryptIndexOffset;
        }
        std::uint8_t resolvedPoolSlot = UINT8_MAX;
        current = stableIndexedPositions[component].Resolve(
            index,
            indexAfter,
            current,
            static_cast<std::uint8_t>(poolSlot),
            resolvedPoolSlot);

        candidates.ring = selected.ring;
        candidates.index = index;
        candidates.decodedPhysicalSlot =
            static_cast<std::uint8_t>(decodedPoolSlot);
        candidates.selectedPhysicalSlot =
            static_cast<std::uint8_t>(poolSlot);
        candidates.decryptIndexOffset =
            resolvedDecryptIndexOffset;
        candidates.decryptIndexEvidence = static_cast<std::uint8_t>(
            std::min<std::size_t>(
                decryptIndexCalibration.Evidence(), UINT8_MAX));
        candidates.poolBlockCount = predictedPoolBlockCount;
        candidates.resolvedPosition = current;
        candidates.resolvedPoolSlot = resolvedPoolSlot;
        candidates.decryptIndexLocked =
            effectiveDecryptIndexOffset !=
            kCoordinatePoolUnknownDecryptIndexOffset;
        candidates.resolvedValid = IsFinitePosition(current);
        probe.decryptIndexOffset = candidates.decryptIndexOffset;
        probe.decryptIndexEvidence = candidates.decryptIndexEvidence;
        probe.poolBlockCount = candidates.poolBlockCount;
        probe.selectedPoolSlot = candidates.resolvedPoolSlot;
        probe.decryptIndexLocked = candidates.decryptIndexLocked;
        if (!candidates.resolvedValid) {
            selected.recovery.Observe(
                CoordinatePoolRingReadEvent::OtherFailure);
            SetError(CoordinatePoolRuntimeError::PositionNotFinite);
            return false;
        }
        selected.recovery.Observe(CoordinatePoolRingReadEvent::Success);

        if (IsCoordinatePoolTraceEnabled()) {
            std::fprintf(
                stderr,
                "[coordinate-pool-indexed] frame=%llu component=%llx "
                "ring=%llx index=%llx index_after=%llx decoded=%llu "
                "offset=%u auto_locked=%d evidence=%u components=%zu "
                "blocks=%u slot=%u stable=%d "
                "address=%llx xyz=(%.3f,%.3f,%.3f)\n",
                static_cast<unsigned long long>(frame),
                static_cast<unsigned long long>(component),
                static_cast<unsigned long long>(selected.ring),
                static_cast<unsigned long long>(index),
                static_cast<unsigned long long>(indexAfter),
                static_cast<unsigned long long>(decodedPoolSlot),
                static_cast<unsigned int>(resolvedDecryptIndexOffset),
                candidates.decryptIndexLocked ? 1 : 0,
                static_cast<unsigned int>(
                    candidates.decryptIndexEvidence),
                decryptIndexCalibration.ComponentCount(),
                static_cast<unsigned int>(predictedPoolBlockCount),
                static_cast<unsigned int>(poolSlot),
                index == indexAfter ? 1 : 0,
                static_cast<unsigned long long>(coordinateAddress),
                current.x,
                current.y,
                current.z);
        }
        return true;
    }

    bool FinishIndexedRemoteReadFailureUnlocked() {
        SetError(CoordinatePoolRuntimeError::PositionReadFailed);
        return false;
    }

    void UpdateSlotLayoutProbeUnlocked() noexcept {
        const CoordinatePoolSlotLayout slotLayout =
            slotLayoutCalibration.Layout();
        probe.decodedSlotMask = slotLayoutCalibration.DecodedMask();
        probe.compactPhaseMask =
            slotLayoutCalibration.CompactPhaseMask();
        probe.extendedPhaseMask =
            slotLayoutCalibration.ExtendedPhaseMask();
        probe.logicalSlotCount = slotLayout.logicalSlotCount;
        probe.physicalSlotCount = slotLayout.physicalSlotCount;
        probe.slotPhase = slotLayout.phase;
        probe.slotLayoutKind = static_cast<std::uint8_t>(slotLayout.kind);
        probe.compactLayoutEvidence = static_cast<std::uint8_t>(
            slotLayoutCalibration.EvidenceCount(
                CoordinatePoolSlotLayoutKind::Compact));
        probe.extendedLayoutEvidence = static_cast<std::uint8_t>(
            slotLayoutCalibration.EvidenceCount(
                CoordinatePoolSlotLayoutKind::Extended));
    }

    void TraceSlotLayoutUnlocked(const char* event,
                                 std::uint64_t component,
                                 std::uint64_t decodedSlot,
                                 std::uint16_t changedMask) const noexcept {
        if (!IsCoordinatePoolTraceEnabled()) return;
        const CoordinatePoolSlotLayout slotLayout =
            slotLayoutCalibration.Layout();
        std::fprintf(
            stderr,
            "[coordinate-pool-layout] frame=%llu event=%s "
            "component=%llx decoded=%llu changed_mask=%04x "
            "kind=%u logical=%u physical=%u phase=%u "
            "decoded_mask=%04x compact_phase_mask=%04x "
            "extended_phase_mask=%04x compact_evidence=%zu "
            "extended_evidence=%zu\n",
            static_cast<unsigned long long>(frame),
            event != nullptr ? event : "unknown",
            static_cast<unsigned long long>(component),
            static_cast<unsigned long long>(decodedSlot),
            static_cast<unsigned int>(changedMask),
            static_cast<unsigned int>(slotLayout.kind),
            static_cast<unsigned int>(slotLayout.logicalSlotCount),
            static_cast<unsigned int>(slotLayout.physicalSlotCount),
            static_cast<unsigned int>(slotLayout.phase),
            static_cast<unsigned int>(slotLayoutCalibration.DecodedMask()),
            static_cast<unsigned int>(
                slotLayoutCalibration.CompactPhaseMask()),
            static_cast<unsigned int>(
                slotLayoutCalibration.ExtendedPhaseMask()),
            slotLayoutCalibration.EvidenceCount(
                CoordinatePoolSlotLayoutKind::Compact),
            slotLayoutCalibration.EvidenceCount(
                CoordinatePoolSlotLayoutKind::Extended));
    }

    void FinishReadUnlocked(bool validationWasRequested) {
        ++probe.successes;
        codeValidationRequested = validationWasRequested;
        ClearReadDiagnosticUnlocked();
        probe.error = CoordinatePoolRuntimeError::None;
        probe.stage = CoordinatePoolRuntimeStage::Active;
    }

    void FinishFailedReadUnlocked(bool validationWasRequested) {
        codeValidationRequested = validationWasRequested ||
            ShouldRequestCoordinatePoolCodeValidationAfterReadFailure(
                probe.error, probe.read);
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

    bool ResolveRootPointerAddressUnlocked(
        std::uint64_t& rootAddress) const noexcept {
        rootAddress = 0;
        std::uint64_t rootBase = 0;
        return memory != nullptr &&
            AddUnsignedOffset(moduleBase, layout.rootRva, rootBase) &&
            AddUnsignedOffset(rootBase, layout.bridgeOffset, rootAddress) &&
            IsRemoteAddress(rootAddress);
    }

    bool ReadAcceptedRootSnapshotUnlocked(
        const CoordinatePoolRootSnapshot& accepted,
        CoordinatePoolRootSnapshot& snapshot) {
        snapshot = {};
        if (!IsCoordinatePoolRootSnapshotInitialized(accepted) ||
            !IsRemoteAddress(accepted.bridge)) {
            return false;
        }

        std::uint64_t rootAddress = 0;
        std::uint64_t contextAddress = 0;
        std::uint64_t entryAddress = 0;
        if (!ResolveRootPointerAddressUnlocked(rootAddress) ||
            !AddSignedOffset(
                accepted.bridge, layout.contextOffset, contextAddress) ||
            !AddUnsignedOffset(
                accepted.bridge, layout.entryOffset, entryAddress) ||
            !IsRemoteAddress(contextAddress) ||
            !IsRemoteAddress(entryAddress)) {
            return false;
        }

        std::uint64_t rawBridgeBefore = 0;
        std::uint64_t rawContext = 0;
        std::uint64_t rawEntry = 0;
        std::uint64_t rawBridgeAfter = 0;
        const std::array<MemoryReadRequest, 4> requests{{
            {rootAddress, &rawBridgeBefore, sizeof(rawBridgeBefore)},
            {contextAddress, &rawContext, sizeof(rawContext)},
            {entryAddress, &rawEntry, sizeof(rawEntry)},
            {rootAddress, &rawBridgeAfter, sizeof(rawBridgeAfter)},
        }};
        const std::array<CoordinateReadStage, 4> stages{{
            CoordinateReadStage::Root,
            CoordinateReadStage::Context,
            CoordinateReadStage::Entry,
            CoordinateReadStage::Root,
        }};
        std::size_t failedIndex = requests.size();
        if (!ReadRemoteBatchUnlocked(
                requests.data(),
                requests.size(),
                stages.data(),
                failedIndex)) {
            return false;
        }

        snapshot = {
            NormalizePointer(rawBridgeBefore),
            rawContext,
            NormalizePointer(rawEntry),
        };
        const std::uint64_t trailingBridge =
            NormalizePointer(rawBridgeAfter);
        return IsValidGuestAddress(snapshot.context) &&
            IsRemoteAddress(snapshot.entry) && (snapshot.entry & 3U) == 0 &&
            CoordinatePoolGuardedRootSnapshotMatches(
                accepted, snapshot, trailingBridge);
    }

    bool ReadIndexedRootSnapshotUnlocked(
        CoordinatePoolRootSnapshot& snapshot) {
        snapshot = {};
        std::uint64_t rootAddress = 0;
        if (!ResolveRootPointerAddressUnlocked(rootAddress)) return false;

        std::uint64_t rawBridge = 0;
        if (!ReadRemoteUnlocked(
                rootAddress,
                &rawBridge,
                sizeof(rawBridge),
                CoordinateReadStage::Root)) {
            return false;
        }
        const std::uint64_t nextBridge = NormalizePointer(rawBridge);
        std::uint64_t contextAddress = 0;
        std::uint64_t entryAddress = 0;
        if (!IsRemoteAddress(nextBridge) ||
            !ResolveCoordinatePoolIndexedRootAddresses(
                rawBridge,
                layout.contextOffset,
                layout.entryOffset,
                contextAddress,
                entryAddress) ||
            !IsRemoteAddress(contextAddress) ||
            !IsRemoteAddress(entryAddress)) {
            return false;
        }

        std::uint64_t rawEntry = 0;
        if (!ReadRemoteUnlocked(
                entryAddress,
                &rawEntry,
                sizeof(rawEntry),
                CoordinateReadStage::Entry)) {
            return false;
        }
        const std::uint64_t nextEntry = NormalizePointer(rawEntry);
        std::uint64_t rawContext = 0;
        if (!ReadRemoteUnlocked(
                contextAddress,
                &rawContext,
                sizeof(rawContext),
                CoordinateReadStage::Context)) {
            return false;
        }
        snapshot = {
            nextBridge,
            rawContext,
            nextEntry,
        };
        return IsValidGuestAddress(snapshot.context) &&
            IsRemoteAddress(snapshot.entry) && (snapshot.entry & 3U) == 0;
    }

    bool ReadRootSnapshotUnlocked(CoordinatePoolRootSnapshot& snapshot) {
        snapshot = {};
        std::uint64_t rootAddress = 0;
        if (!ResolveRootPointerAddressUnlocked(rootAddress)) return false;
        std::uint64_t rawBridge = 0;
        if (!ReadRemoteUnlocked(
                rootAddress,
                &rawBridge,
                sizeof(rawBridge),
                CoordinateReadStage::Root)) {
            return false;
        }
        const std::uint64_t nextBridge = NormalizePointer(rawBridge);
        std::uint64_t rawContext = 0;
        std::uint64_t rawEntry = 0;
        std::uint64_t contextAddress = 0;
        std::uint64_t entryAddress = 0;
        if (!IsRemoteAddress(nextBridge) ||
            !AddSignedOffset(
                nextBridge, layout.contextOffset, contextAddress) ||
            layout.entryOffset >
                std::numeric_limits<std::uint64_t>::max() - nextBridge ||
            !IsRemoteAddress(contextAddress) ||
            !AddUnsignedOffset(
                nextBridge, layout.entryOffset, entryAddress) ||
            !IsRemoteAddress(entryAddress)) {
            return false;
        }
        const std::array<MemoryReadRequest, 2> requests{{
            {contextAddress, &rawContext, sizeof(rawContext)},
            {entryAddress, &rawEntry, sizeof(rawEntry)},
        }};
        const std::array<CoordinateReadStage, 2> stages{{
            CoordinateReadStage::Context,
            CoordinateReadStage::Entry,
        }};
        std::size_t failedIndex = requests.size();
        if (!ReadRemoteBatchUnlocked(
                requests.data(),
                requests.size(),
                stages.data(),
                failedIndex)) {
            // A device or kernel may reject multi-iovec reads.  Retain the
            // scalar path as a compatibility fallback for that case.
            ClearReadDiagnosticUnlocked();
            if (!ReadRemoteUnlocked(
                    contextAddress,
                    &rawContext,
                    sizeof(rawContext),
                    CoordinateReadStage::Context) ||
                !ReadRemoteUnlocked(
                    entryAddress,
                    &rawEntry,
                    sizeof(rawEntry),
                    CoordinateReadStage::Entry)) {
                return false;
            }
        }
        snapshot = {
            nextBridge,
            rawContext,
            NormalizePointer(rawEntry),
        };
        return IsValidGuestAddress(snapshot.context) &&
            IsRemoteAddress(snapshot.entry) && (snapshot.entry & 3U) == 0;
    }

    bool ResolveRootUnlocked(
        const CoordinatePoolRootSnapshot& accepted,
        CoordinatePoolRootSnapshot& snapshot,
        int& status) {
        snapshot = {};
        status = 0;
        if (indexedPointers) {
            if (ReadIndexedRootSnapshotUnlocked(snapshot)) {
                ClearReadDiagnosticUnlocked();
                return true;
            }
            status = probe.read.HasFailure()
                ? probe.systemError
                : -EAGAIN;
            return false;
        }
        if (ReadAcceptedRootSnapshotUnlocked(accepted, snapshot)) {
            ClearReadDiagnosticUnlocked();
            return true;
        }
        ClearReadDiagnosticUnlocked();
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
        probe.analysisFindStage = 0;
        probe.analysisFindDetail = 0;
        probe.analysisMaddCount = 0;
        probe.analysisRingMaddCount = 0;
        probe.analysisCandidateCount = 0;
        probe.analysisFailureInstruction = 0;
        std::uint64_t mappingStart = 0;
        std::uint64_t mappingEnd = 0;
        int mappingStatus = 0;
        ExecutableMappingIndex mappingIndex{};
        if (!BuildExecutableMappingIndex(
                processId,
                guestEntry,
                mappingIndex,
                &mappingStatus)) {
            SetError(
                CoordinatePoolRuntimeError::EntryMappingMissing,
                true,
                mappingStatus);
            return false;
        }
        mappingStart = mappingIndex.windowStart;
        mappingEnd = mappingIndex.windowEnd;
        probe.executableMappingFragments =
            static_cast<std::uint32_t>(std::min<std::size_t>(
                mappingIndex.fragmentCount,
                std::numeric_limits<std::uint32_t>::max()));
        probe.executableMappingStart = mappingStart;
        probe.executableMappingEnd = mappingEnd;
        probe.failedMethod = 0;
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
                diagnostic.address == (guestEntry & kPageMask)
                    ? CoordinatePoolRuntimeError::EntryPageReadFailed
                    : CoordinatePoolRuntimeError::CodeReadFailed,
                true,
                diagnostic.systemError);
            return false;
        };

        auto loadPage = [&](std::uint64_t pageAddress) {
            if (pageAddress < mappingStart || pageAddress >= mappingEnd ||
                (pageAddress & (kPageSize - 1)) != 0 ||
                !mappingIndex.ContainsPage(pageAddress)) {
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
                int refreshStatus = 0;
                ExecutableMappingIndex refreshedIndex{};
                const bool mappingFound = BuildExecutableMappingIndex(
                    processId,
                    guestEntry,
                    refreshedIndex,
                    &refreshStatus);
                if (mappingFound &&
                    !SameExecutableMappingIndex(mappingIndex, refreshedIndex)) {
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
                probe.failedMethod = methodAddress;
                return false;
            }
            if (!mappingIndex.ContainsPage(methodAddress & kPageMask)) {
                probe.failedMethod = methodAddress;
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
                if (!loadPage(pageAddress)) {
                    probe.failedMethod = methodAddress;
                    return false;
                }
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
                SetError(
                    probe.failedMethod == guestEntry
                        ? CoordinatePoolRuntimeError::EntryMappingFragmented
                        : CoordinatePoolRuntimeError::AnalysisFailed,
                    true,
                    probe.failedMethod == guestEntry ? -ERANGE : -ENODATA);
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
            probe.analysisFindStage = static_cast<std::uint8_t>(
                candidate->failure_stage());
            probe.analysisFindDetail = static_cast<std::uint8_t>(
                candidate->failure_detail());
            probe.analysisMaddCount = candidate->madd_count();
            probe.analysisRingMaddCount = candidate->ring_madd_count();
            probe.analysisCandidateCount = candidate->candidate_count();
            probe.analysisFailureInstruction =
                candidate->failure_instruction();
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
                if (probe.failedMethod != 0 &&
                    probe.failedMethod == requestedAddress) {
                    SetError(
                        CoordinatePoolRuntimeError::EntryMappingFragmented,
                        true,
                        -ERANGE);
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
        executableMappingIndex = std::move(mappingIndex);
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
        finder->compact_runtime_plan();
        ClearReadDiagnosticUnlocked();
        probe.analysisFindStage = 0;
        probe.analysisFindDetail = 0;
        probe.analysisMaddCount = 0;
        probe.analysisRingMaddCount = 0;
        probe.analysisCandidateCount = 0;
        probe.analysisFailureInstruction = 0;
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
            guestPage - codeBase >= codeSize ||
            !executableMappingIndex.ContainsPage(guestPage)) {
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
            !IsCoordinatePoolReadRangeValid(remotePage, kPageSize)) {
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
            int refreshStatus = 0;
            ExecutableMappingIndex refreshedIndex{};
            const bool mappingFound = BuildExecutableMappingIndex(
                processId,
                guestEntry,
                refreshedIndex,
                &refreshStatus);
            if (mappingFound &&
                !SameExecutableMappingIndex(
                    executableMappingIndex, refreshedIndex)) {
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
        parameterComponent = component;
        parameterFrame = frame;
        parameterFingerprint = CoordinatePoolParameterFingerprint(*finder);
        if (IsCoordinatePoolTraceEnabled()) {
            std::fprintf(
                stderr,
                "[coordinate-pool-parameters] frame=%llu component=%llx "
                "fingerprint=%llx mem=%zu var=%zu\n",
                static_cast<unsigned long long>(frame),
                static_cast<unsigned long long>(parameterComponent),
                static_cast<unsigned long long>(parameterFingerprint),
                finder->mem_param_list.size(),
                finder->analyze.varParams.size());
            for (const auto& parameter : finder->mem_param_list) {
                std::fprintf(
                    stderr,
                    "[coordinate-pool-parameter] kind=mem name=%s size=%u "
                    "disp=%d value=%llx offsets=%zu\n",
                    parameter.name.c_str(),
                    parameter.size,
                    parameter.disp,
                    static_cast<unsigned long long>(parameter.value),
                    parameter.offset.size());
            }
            for (const auto& parameter : finder->analyze.varParams) {
                std::fprintf(
                    stderr,
                    "[coordinate-pool-parameter] kind=var name=%s addr=%llx "
                    "reg=%u value=%llx\n",
                    parameter.name.c_str(),
                    static_cast<unsigned long long>(parameter.addr),
                    static_cast<unsigned int>(parameter.reg),
                    static_cast<unsigned long long>(parameter.value));
            }
            std::fflush(stderr);
        }
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
        parameterComponent = 0;
        parameterFrame = std::numeric_limits<std::uint64_t>::max();
        parameterFingerprint = 0;
        computedContext = 0;
        ClearPoolPointerUnlocked();
        poolPointerRefreshFrame =
            std::numeric_limits<std::uint64_t>::max();
        return ClearDynamicPagesUnlocked();
    }

    void ClearPoolPointerUnlocked() {
        lastPoolPointer = 0;
        predictedPoolBlockCount = 0;
        ringSlots.clear();
        stableIndexedPositions.clear();
        ringSearchBudget.Reset();
        ResetDecryptIndexWitnessesUnlocked();
        // The slot layout belongs to the analyzed code, not a rotating
        // execution context or pool allocation.
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
            if (IsCoordinatePoolTraceEnabled()) {
                std::fprintf(
                    stderr,
                    "[coordinate-pool-ring-event] frame=%llu "
                    "component=0 event=pool_changed previous=%llx "
                    "pool=%llx cleared=%zu\n",
                    static_cast<unsigned long long>(frame),
                    static_cast<unsigned long long>(lastPoolPointer),
                    static_cast<unsigned long long>(pointer),
                    ringSlots.size());
            }
            predictedPoolBlockCount = 0;
            ringSlots.clear();
            stableIndexedPositions.clear();
            ringSearchBudget.Reset();
            ResetDecryptIndexWitnessesUnlocked();
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

    bool ReadRemoteBatchUnlocked(
        const MemoryReadRequest* requests,
        std::size_t count,
        const CoordinateReadStage* stages,
        std::size_t& failedIndex) {
        CoordinateReadDiagnostic diagnostic{};
        bool read = false;
        failedIndex = count;
        if (memory != nullptr) {
            read = memory->ReadCoordinateMemoryBatch(
                requests, count, diagnostic, failedIndex);
        } else {
            failedIndex = 0;
            if (requests != nullptr && count != 0) {
                diagnostic.address = requests[0].remoteAddress;
                diagnostic.size = requests[0].size;
            }
            diagnostic.failure =
                CoordinateReadFailure::TransportUnavailable;
            diagnostic.systemError = -ENODEV;
        }
        if (!read) {
            const std::size_t stageIndex = failedIndex < count
                ? failedIndex
                : 0;
            diagnostic.stage = stages != nullptr && count != 0
                ? stages[stageIndex]
                : CoordinateReadStage::None;
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
            IsCoordinatePoolReadRangeValid(remotePage, kPageSize) &&
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
        executableMappingIndex = {};
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
        parameterComponent = 0;
        parameterFrame = std::numeric_limits<std::uint64_t>::max();
        parameterFingerprint = 0;
        lastPoolPointer = 0;
        predictedPoolBlockCount = 0;
        decryptIndexCalibrationBlockCount = 0;
        decryptIndexCalibration.Reset();
        decryptIndexCalibrationFrame =
            std::numeric_limits<std::uint64_t>::max();
        decryptIndexCalibrationReads = 0;
        decryptIndexCalibrationVisitCount = 0;
        decryptIndexCalibrationPreviousVisitCount = 0;
        decryptIndexCalibrationWindowStart = 0;
        decryptIndexCalibrationWitnesses = {};
        decryptIndexCalibrationWitnessLastSeenFrames = {};
        decryptIndexCalibrationWitnessCount = 0;
        decryptIndexCalibrationProgressFrame =
            std::numeric_limits<std::uint64_t>::max();
        decryptIndexCalibrationLastEvidence = 0;
        effectiveDecryptIndexOffset =
            kCoordinatePoolUnknownDecryptIndexOffset;
        pendingDecryptIndexOffset =
            kCoordinatePoolUnknownDecryptIndexOffset;
        pendingDecryptIndexFrame =
            std::numeric_limits<std::uint64_t>::max();
        poolPointerRefreshFrame =
            std::numeric_limits<std::uint64_t>::max();
        ringSlots.clear();
        stableIndexedPositions.clear();
        ringSearchBudget.Reset();
        slotLayoutCalibration.Reset();
        probe.bridge = 0;
        probe.context = 0;
        probe.guestEntry = 0;
        probe.entryInstruction = 0;
        probe.codeBase = 0;
        probe.codeSize = 0;
        probe.executableMappingFragments = 0;
        probe.executableMappingStart = 0;
        probe.executableMappingEnd = 0;
        probe.failedMethod = 0;
        probe.analysisFindStage = 0;
        probe.analysisFindDetail = 0;
        probe.analysisMaddCount = 0;
        probe.analysisRingMaddCount = 0;
        probe.analysisCandidateCount = 0;
        probe.analysisFailureInstruction = 0;
        probe.poolPointerOffset = 0;
        probe.indexOffset = 0;
        probe.ringOffset = 0;
        probe.decodedSlotMask = 0;
        probe.compactPhaseMask = 0;
        probe.extendedPhaseMask = 0;
        probe.logicalSlotCount = 0;
        probe.physicalSlotCount = 0;
        probe.slotPhase = 0;
        probe.slotLayoutKind = 0;
        probe.compactLayoutEvidence = 0;
        probe.extendedLayoutEvidence = 0;
        probe.decryptIndexOffset = 0;
        probe.decryptIndexEvidence = 0;
        probe.poolBlockCount = 0;
        probe.selectedPoolSlot = 0;
        probe.decryptIndexLocked = false;
        UpdateSlotLayoutProbeUnlocked();
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
        indexedPointers = false;
        finder.reset();
        analysisCodeFingerprints.clear();
        executableMappingIndex = {};
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
        parameterComponent = 0;
        parameterFrame = std::numeric_limits<std::uint64_t>::max();
        parameterFingerprint = 0;
        analysisInvalidated = false;
        nextCodeValidationFrame = 0;
        codeValidationRequested = false;
        frame = 0;
        lastPoolPointer = 0;
        predictedPoolBlockCount = 0;
        decryptIndexCalibrationBlockCount = 0;
        decryptIndexCalibration.Reset();
        decryptIndexCalibrationFrame =
            std::numeric_limits<std::uint64_t>::max();
        decryptIndexCalibrationReads = 0;
        decryptIndexCalibrationVisitCount = 0;
        decryptIndexCalibrationPreviousVisitCount = 0;
        decryptIndexCalibrationWindowStart = 0;
        decryptIndexCalibrationWitnesses = {};
        decryptIndexCalibrationWitnessLastSeenFrames = {};
        decryptIndexCalibrationWitnessCount = 0;
        decryptIndexCalibrationProgressFrame =
            std::numeric_limits<std::uint64_t>::max();
        decryptIndexCalibrationLastEvidence = 0;
        effectiveDecryptIndexOffset =
            kCoordinatePoolUnknownDecryptIndexOffset;
        pendingDecryptIndexOffset =
            kCoordinatePoolUnknownDecryptIndexOffset;
        pendingDecryptIndexFrame =
            std::numeric_limits<std::uint64_t>::max();
        poolPointerRefreshFrame =
            std::numeric_limits<std::uint64_t>::max();
        ringSlots.clear();
        stableIndexedPositions.clear();
        ringSearchBudget.Reset();
        slotLayoutCalibration.Reset();
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
    std::uint64_t parameterComponent = 0;
    std::uint64_t parameterFrame =
        std::numeric_limits<std::uint64_t>::max();
    std::uint64_t parameterFingerprint = 0;
    bool analysisInvalidated = false;
    std::uint64_t frame = 0;
    CoordinateReadDiagnostic toleratedReadFailure{};
    std::uint64_t lastPoolPointer = 0;
    std::uint8_t predictedPoolBlockCount = 0;
    std::uint8_t decryptIndexCalibrationBlockCount = 0;
    CoordinatePoolDecryptIndexCalibration decryptIndexCalibration{};
    std::uint64_t decryptIndexCalibrationFrame =
        std::numeric_limits<std::uint64_t>::max();
    std::size_t decryptIndexCalibrationReads = 0;
    std::size_t decryptIndexCalibrationVisitCount = 0;
    std::size_t decryptIndexCalibrationPreviousVisitCount = 0;
    std::size_t decryptIndexCalibrationWindowStart = 0;
    std::array<std::uint64_t,
               kCoordinatePoolDecryptIndexCalibrationReadsPerFrame>
        decryptIndexCalibrationWitnesses{};
    std::array<std::uint64_t,
               kCoordinatePoolDecryptIndexCalibrationReadsPerFrame>
        decryptIndexCalibrationWitnessLastSeenFrames{};
    std::size_t decryptIndexCalibrationWitnessCount = 0;
    std::uint64_t decryptIndexCalibrationProgressFrame =
        std::numeric_limits<std::uint64_t>::max();
    std::size_t decryptIndexCalibrationLastEvidence = 0;
    std::uint8_t effectiveDecryptIndexOffset =
        kCoordinatePoolUnknownDecryptIndexOffset;
    std::uint8_t pendingDecryptIndexOffset =
        kCoordinatePoolUnknownDecryptIndexOffset;
    std::uint64_t pendingDecryptIndexFrame =
        std::numeric_limits<std::uint64_t>::max();
    std::uint64_t poolPointerRefreshFrame =
        std::numeric_limits<std::uint64_t>::max();
    std::unordered_map<std::uint64_t, RingSlot> ringSlots;
    std::unordered_map<std::uint64_t, CoordinatePoolStablePositionCache>
        stableIndexedPositions;
    CoordinatePoolRingSearchBudget ringSearchBudget;
    CoordinatePoolSlotLayoutCalibration slotLayoutCalibration;
    std::vector<std::uint8_t> poolSnapshotScratch;
    std::unique_ptr<pool::coord_dec::FindDec> finder;
    std::vector<CodeRangeFingerprint> analysisCodeFingerprints;
    ExecutableMappingIndex executableMappingIndex{};
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
    bool indexedPointers = false;
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
    std::uint64_t frame,
    bool indexedPointers) noexcept {
    try {
        return impl_ != nullptr && impl_->Refresh(
            memory,
            processId,
            moduleBase,
            executionContext,
            frame,
            indexedPointers);
    } catch (...) {
        return false;
    }
}

bool CoordinatePoolRuntime::ReadPosition(
    std::uintptr_t component,
    CoordinatePoolPosition& position,
    bool forceRefresh) noexcept {
    position = {};
    try {
        return impl_ != nullptr && impl_->ReadPosition(
            component, position, forceRefresh);
    } catch (...) {
        position = {};
        return false;
    }
}

bool CoordinatePoolRuntime::ReadPosition(
    std::uintptr_t component,
    std::uint32_t decryptIndexOffset,
    CoordinatePoolPosition& position,
    bool forceRefresh) noexcept {
    position = {};
    try {
        return impl_ != nullptr && impl_->ReadPosition(
            component, decryptIndexOffset, position, forceRefresh);
    } catch (...) {
        position = {};
        return false;
    }
}

bool CoordinatePoolRuntime::ReadCandidates(
    std::uintptr_t component,
    CoordinatePoolCandidateSet& candidates,
    bool forceRefresh) noexcept {
    candidates = {};
    try {
        return impl_ != nullptr && impl_->ReadCandidates(
            component, candidates, forceRefresh);
    } catch (...) {
        candidates = {};
        return false;
    }
}

bool CoordinatePoolRuntime::ReadCandidates(
    std::uintptr_t component,
    std::uint32_t decryptIndexOffset,
    CoordinatePoolCandidateSet& candidates,
    bool forceRefresh) noexcept {
    candidates = {};
    try {
        return impl_ != nullptr && impl_->ReadCandidates(
            component, decryptIndexOffset, candidates, forceRefresh);
    } catch (...) {
        candidates = {};
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
