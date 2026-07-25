#include "game/native/CoordinateExecutionCandidate.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

namespace lengjing::game::native {
namespace {

constexpr std::uint64_t kProfile12RootOffset = UINT64_C(0x0E9CC2EC);
constexpr std::uint64_t kDefaultRootOffset = UINT64_C(0x0E7F7664);
constexpr std::uint64_t kProfile12PointerOffset = UINT64_C(0x8);
constexpr std::uint64_t kDefaultPointerOffset = UINT64_C(0xC);
constexpr std::uint64_t kEntryPointerOffset = UINT64_C(0xA0);
constexpr std::uint64_t kScanWindowSize = UINT64_C(0x100000);
constexpr std::uint64_t kScanOverlap = UINT64_C(8);
constexpr std::size_t kReadPageSize = 4096;
constexpr std::size_t kThunkProbeSize = 512;
constexpr std::uint32_t kLdrLiteral64Mask = UINT32_C(0xFF000000);
constexpr std::uint32_t kLdrLiteral64Value = UINT32_C(0x58000000);
constexpr std::uint32_t kAdrpMask = UINT32_C(0x9F000000);
constexpr std::uint32_t kAdrpValue = UINT32_C(0x90000000);
constexpr std::uint32_t kLdrUnsigned64Mask = UINT32_C(0xFFC00000);
constexpr std::uint32_t kLdrUnsigned64Value = UINT32_C(0xF9400000);
constexpr std::uint32_t kBranchRegisterMask = UINT32_C(0xFFFFFC1F);
constexpr std::uint32_t kBranchRegisterValue = UINT32_C(0xD61F0000);

using CandidateSink =
    std::function<bool(const CoordinateExecutionCandidate& candidate)>;

bool AddNoOverflow(std::uint64_t left,
                   std::uint64_t right,
                   std::uint64_t* output) noexcept {
    if (output == nullptr ||
        right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    *output = left + right;
    return true;
}

bool AddSigned(std::uint64_t base,
               std::int64_t displacement,
               std::uint64_t* output) noexcept {
    if (displacement >= 0) {
        return AddNoOverflow(
            base, static_cast<std::uint64_t>(displacement), output);
    }
    const std::uint64_t magnitude =
        static_cast<std::uint64_t>(-(displacement + 1)) + 1;
    if (output == nullptr || magnitude > base) return false;
    *output = base - magnitude;
    return true;
}

std::int64_t SignExtend(std::uint64_t value, unsigned bits) noexcept {
    const std::uint64_t sign = std::uint64_t{1} << (bits - 1);
    const std::uint64_t mask = (std::uint64_t{1} << bits) - 1;
    value &= mask;
    if ((value & sign) == 0) return static_cast<std::int64_t>(value);
    return -static_cast<std::int64_t>(((~value) & mask) + 1);
}

std::uint64_t UntagPointer(std::uint64_t value) noexcept {
    return value & kCoordinateExecutionPointerMask;
}

bool IsCanonicalPointer(std::uint64_t value) noexcept {
    const std::uint64_t pointer = UntagPointer(value);
    return pointer >= kCoordinateExecutionPointerMin &&
        pointer <= kCoordinateExecutionPointerMax;
}

template <typename T>
bool ReadValue(const CoordinateExecutionReadCallback& read,
               std::uint64_t address,
               T* value) {
    if (!read || value == nullptr) return false;
    *value = T{};
    return read(address, value, sizeof(T));
}

bool GetModuleEnd(const CoordinateExecutionModuleSnapshot& module,
                  std::uint64_t* end) noexcept {
    return module.size != 0 &&
        module.size <= std::numeric_limits<std::uint64_t>::max() &&
        AddNoOverflow(
            module.guestBase, static_cast<std::uint64_t>(module.size), end);
}

bool IsInModule(const CoordinateExecutionModuleSnapshot& module,
                std::uint64_t moduleEnd,
                std::uint64_t address) noexcept {
    return address >= module.guestBase && address < moduleEnd;
}

bool IsBranchRegister(std::uint32_t instruction) noexcept {
    return (instruction & kBranchRegisterMask) == kBranchRegisterValue;
}

bool ResolveLdrLiteral(const CoordinateExecutionReadCallback& read,
                       std::uint64_t pc,
                       std::uint32_t instruction,
                       int requiredRegister,
                       std::uint64_t* value,
                       unsigned* destinationRegister) {
    if (value == nullptr || destinationRegister == nullptr ||
        (instruction & kLdrLiteral64Mask) != kLdrLiteral64Value) {
        return false;
    }

    const unsigned destination = instruction & 0x1FU;
    if (requiredRegister >= 0 &&
        destination != static_cast<unsigned>(requiredRegister)) {
        return false;
    }

    const std::int64_t immediate =
        SignExtend((instruction >> 5U) & 0x7FFFFU, 19) * 4;
    std::uint64_t literalAddress = 0;
    if (!AddSigned(pc, immediate, &literalAddress) ||
        !ReadValue(read, literalAddress, value)) {
        return false;
    }

    *destinationRegister = destination;
    return true;
}

bool ResolveAdrpLdr(const CoordinateExecutionReadCallback& read,
                    std::uint64_t pc,
                    std::uint32_t adrp,
                    std::uint32_t ldr,
                    unsigned requiredRegister,
                    std::uint64_t* value) {
    if (value == nullptr || (adrp & kAdrpMask) != kAdrpValue ||
        (ldr & kLdrUnsigned64Mask) != kLdrUnsigned64Value) {
        return false;
    }

    const unsigned adrpRegister = adrp & 0x1FU;
    const unsigned baseRegister = (ldr >> 5U) & 0x1FU;
    const unsigned targetRegister = ldr & 0x1FU;
    if (adrpRegister != baseRegister || adrpRegister != targetRegister ||
        targetRegister != requiredRegister) {
        return false;
    }

    const std::uint64_t immediateBits =
        (static_cast<std::uint64_t>((adrp >> 5U) & 0x7FFFFU) << 2U) |
        ((adrp >> 29U) & 3U);
    const std::int64_t pageDelta = SignExtend(immediateBits, 21) * 4096;
    std::uint64_t address = 0;
    if (!AddSigned(pc & ~std::uint64_t{0xFFF}, pageDelta, &address) ||
        !AddNoOverflow(
            address,
            static_cast<std::uint64_t>((ldr >> 10U) & 0xFFFU) * 8U,
            &address) ||
        !ReadValue(read, address, value)) {
        return false;
    }
    return true;
}

bool AnalyzeExternalThunk(const CoordinateExecutionReadCallback& read,
                          const CoordinateExecutionModuleSnapshot& module,
                          std::uint64_t moduleEnd,
                          std::uint64_t thunk,
                          std::uint64_t relativeEntry,
                          std::uint64_t* q0) {
    if (q0 == nullptr) return false;
    const std::uint64_t entry = UntagPointer(thunk);
    if (!IsCanonicalPointer(entry) || IsInModule(module, moduleEnd, entry)) {
        return false;
    }

    std::array<std::byte, kThunkProbeSize> bytes{};
    if (!read(entry, bytes.data(), bytes.size())) return false;

    std::uint64_t matchedRelativeEntry = 0;
    std::uint64_t externalX0 = 0;
    for (std::size_t offset = 0;
         offset + 12 <= bytes.size();
         offset += 4) {
        std::uint32_t first = 0;
        std::uint32_t second = 0;
        std::uint32_t third = 0;
        std::memcpy(&first, bytes.data() + offset, sizeof(first));
        std::memcpy(&second, bytes.data() + offset + 4, sizeof(second));
        std::memcpy(&third, bytes.data() + offset + 8, sizeof(third));

        if (IsBranchRegister(first) && matchedRelativeEntry != 0) break;

        std::uint64_t loadedValue = 0;
        unsigned loadedRegister = first & 0x1FU;
        std::uint64_t instructionPc = 0;
        if (!AddNoOverflow(
                entry, static_cast<std::uint64_t>(offset), &instructionPc)) {
            return false;
        }

        bool resolved = ResolveLdrLiteral(
            read,
            instructionPc,
            first,
            -1,
            &loadedValue,
            &loadedRegister);
        if (!resolved && IsBranchRegister(third)) {
            const unsigned branchRegister = (third >> 5U) & 0x1FU;
            resolved = ResolveAdrpLdr(
                read,
                instructionPc,
                first,
                second,
                branchRegister,
                &loadedValue);
            if (resolved) loadedRegister = branchRegister;
        }

        if (!resolved) continue;
        const std::uint64_t untagged = UntagPointer(loadedValue);
        if (IsInModule(module, moduleEnd, untagged)) {
            matchedRelativeEntry = untagged - module.guestBase;
        } else if (loadedRegister == 0 && IsCanonicalPointer(loadedValue)) {
            externalX0 = loadedValue;
        }
    }

    if (matchedRelativeEntry != relativeEntry || externalX0 == 0) {
        return false;
    }
    *q0 = externalX0;
    return true;
}

bool InspectVeneer(const CoordinateExecutionReadCallback& read,
                   const CoordinateExecutionModuleSnapshot& module,
                   std::uint64_t moduleEnd,
                   std::uint64_t ldrPc,
                   std::uint64_t relativeEntry,
                   CoordinateExecutionCandidate* candidate) {
    if (candidate == nullptr || ldrPc < module.guestBase ||
        ldrPc - module.guestBase < 4) {
        return false;
    }
    const std::uint64_t returnStub = ldrPc - 4;
    if ((returnStub & 3U) != 0 ||
        !IsInModule(module, moduleEnd, returnStub) ||
        moduleEnd - returnStub < 12) {
        return false;
    }

    const std::uint32_t ldr =
        static_cast<std::uint32_t>(kCoordinateExecutionReturnStubMagic);
    std::uint64_t thunk = 0;
    unsigned destination = 0;
    if (!ResolveLdrLiteral(
            read, ldrPc, ldr, 16, &thunk, &destination)) {
        return false;
    }

    std::uint64_t magic = 0;
    if (!ReadValue(read, ldrPc, &magic) ||
        magic != kCoordinateExecutionReturnStubMagic) {
        return false;
    }

    const std::uint64_t entry = UntagPointer(thunk);
    std::uint64_t q0 = 0;
    if (!AnalyzeExternalThunk(
            read, module, moduleEnd, entry, relativeEntry, &q0)) {
        return false;
    }

    std::uint64_t stableMagic = 0;
    if (!ReadValue(read, ldrPc, &stableMagic) || stableMagic != magic) {
        return false;
    }

    *candidate = CoordinateExecutionCandidate{
        q0,
        relativeEntry,
        entry,
        returnStub,
    };
    return true;
}

bool ScanRange(const CoordinateExecutionReadCallback& read,
               const CoordinateExecutionModuleSnapshot& module,
               std::uint64_t moduleEnd,
               std::uint64_t begin,
               std::uint64_t end,
               std::uint64_t relativeEntry,
               const CandidateSink& sink) {
    if (begin >= end) return false;

    std::vector<std::byte> buffer;
    std::uint64_t cursor = begin;
    while (cursor < end) {
        const std::uint64_t remaining = end - cursor;
        const std::size_t chunkSize = static_cast<std::size_t>(
            std::min<std::uint64_t>(kScanWindowSize, remaining));
        buffer.assign(chunkSize, std::byte{0});

        bool anyRead = false;
        for (std::size_t offset = 0; offset < chunkSize;) {
            const std::size_t part =
                std::min(kReadPageSize, chunkSize - offset);
            if (read(cursor + offset, buffer.data() + offset, part)) {
                anyRead = true;
            }
            offset += part;
        }

        if (anyRead) {
            for (std::size_t offset = 0; offset + 8 <= chunkSize;
                 offset += 4) {
                std::uint64_t magic = 0;
                std::memcpy(
                    &magic, buffer.data() + offset, sizeof(magic));
                if (magic != kCoordinateExecutionReturnStubMagic) continue;

                std::uint64_t ldrPc = 0;
                CoordinateExecutionCandidate candidate{};
                if (AddNoOverflow(
                        cursor, static_cast<std::uint64_t>(offset), &ldrPc) &&
                    InspectVeneer(
                        read,
                        module,
                        moduleEnd,
                        ldrPc,
                        relativeEntry,
                        &candidate) &&
                    !sink(candidate)) {
                    return true;
                }
            }

            if (chunkSize < 9 || cursor >= end - chunkSize) break;
            cursor += chunkSize - kScanOverlap;
        } else {
            cursor += chunkSize;
        }
    }
    return false;
}

bool ScanInPriorityOrder(
    const CoordinateExecutionReadCallback& read,
    const CoordinateExecutionModuleSnapshot& module,
    std::uint64_t scanAnchor,
    std::uint64_t relativeEntry,
    const CandidateSink& sink) {
    if (!read || relativeEntry == 0 || relativeEntry >= module.size) {
        return false;
    }

    std::uint64_t moduleEnd = 0;
    if (!GetModuleEnd(module, &moduleEnd)) return false;

    if (IsInModule(module, moduleEnd, scanAnchor) &&
        moduleEnd - scanAnchor >= 16 &&
        ScanRange(
            read,
            module,
            moduleEnd,
            scanAnchor,
            scanAnchor + 16,
            relativeEntry,
            sink)) {
        return true;
    }

    const std::uint64_t alignedAnchor =
        scanAnchor & ~(kScanWindowSize - 1);
    const std::uint64_t centerBegin =
        std::max(module.guestBase, alignedAnchor);
    std::uint64_t centerEnd = moduleEnd;
    std::uint64_t proposedEnd = 0;
    if (AddNoOverflow(centerBegin, kScanWindowSize, &proposedEnd)) {
        centerEnd = std::min(moduleEnd, proposedEnd);
    }

    const std::uint64_t firstBegin =
        centerBegin - module.guestBase < kScanOverlap
        ? module.guestBase
        : centerBegin - kScanOverlap;
    const std::uint64_t firstEnd =
        centerEnd >= moduleEnd || moduleEnd - centerEnd < kScanOverlap
        ? moduleEnd
        : centerEnd + kScanOverlap;
    if (ScanRange(
            read,
            module,
            moduleEnd,
            firstBegin,
            firstEnd,
            relativeEntry,
            sink)) {
        return true;
    }
    if (module.guestBase < centerBegin &&
        ScanRange(
            read,
            module,
            moduleEnd,
            module.guestBase,
            centerBegin,
            relativeEntry,
            sink)) {
        return true;
    }
    return centerEnd < moduleEnd &&
        ScanRange(
            read,
            module,
            moduleEnd,
            centerEnd,
            moduleEnd,
            relativeEntry,
            sink);
}

bool NormalizeRelativeEntry(
    const CoordinateExecutionModuleSnapshot& module,
    std::uint64_t rawEntry,
    std::uint64_t* relativeEntry) noexcept {
    if (relativeEntry == nullptr) return false;
    std::uint64_t moduleEnd = 0;
    if (!GetModuleEnd(module, &moduleEnd)) return false;

    const std::uint64_t value = UntagPointer(rawEntry);
    std::uint64_t offset = 0;
    if (IsInModule(module, moduleEnd, value)) {
        offset = value - module.guestBase;
    } else if ((value & 3U) == 0 && value < module.size) {
        offset = value;
    }

    if (offset == 0 || offset >= module.size) return false;
    *relativeEntry = offset;
    return true;
}

bool ResolveDiscoveryInput(
    const CoordinateExecutionReadCallback& read,
    const CoordinateExecutionModuleSnapshot& module,
    std::uint64_t configuredModuleBase,
    std::uint32_t scanProfile,
    std::uint64_t* scanAnchor,
    std::uint64_t* relativeEntry) {
    if (!read || scanAnchor == nullptr || relativeEntry == nullptr) {
        return false;
    }

    const CoordinateExecutionProfileOffsets profile =
        GetCoordinateExecutionProfileOffsets(scanProfile);
    std::uint64_t pointerAddress = 0;
    if (!AddNoOverflow(
            configuredModuleBase, profile.rootOffset, scanAnchor) ||
        !AddNoOverflow(
            *scanAnchor, profile.pointerOffset, &pointerAddress)) {
        return false;
    }

    std::uint64_t root = 0;
    if (!ReadValue(read, pointerAddress, &root)) return false;
    root = UntagPointer(root);
    if (!IsCanonicalPointer(root)) return false;

    std::uint64_t entryAddress = 0;
    std::uint64_t rawEntry = 0;
    if (!AddNoOverflow(root, profile.entryOffset, &entryAddress) ||
        !ReadValue(read, entryAddress, &rawEntry)) {
        return false;
    }
    return NormalizeRelativeEntry(module, rawEntry, relativeEntry);
}

}  // namespace

CoordinateExecutionProfileOffsets GetCoordinateExecutionProfileOffsets(
    std::uint32_t scanProfile) noexcept {
    if (scanProfile == 1 || scanProfile == 2) {
        return CoordinateExecutionProfileOffsets{
            kProfile12RootOffset,
            kProfile12PointerOffset,
            kEntryPointerOffset,
        };
    }
    return CoordinateExecutionProfileOffsets{
        kDefaultRootOffset,
        kDefaultPointerOffset,
        kEntryPointerOffset,
    };
}

CoordinateExecutionCandidateScanResult ScanCoordinateExecutionCandidates(
    const CoordinateExecutionReadCallback& read,
    const CoordinateExecutionModuleSnapshot& module,
    std::uint64_t scanAnchor,
    std::uint64_t relativeEntry) {
    CoordinateExecutionCandidateScanResult result{};
    result.candidates.reserve(16);
    const CandidateSink sink = [&result](
                                   const CoordinateExecutionCandidate& value) {
        if (std::find(
                result.candidates.begin(),
                result.candidates.end(),
                value) != result.candidates.end()) {
            return true;
        }
        if (result.candidates.size() >=
            kCoordinateExecutionCandidateLimit) {
            result.truncated = true;
            return false;
        }
        result.candidates.push_back(value);
        return true;
    };
    static_cast<void>(ScanInPriorityOrder(
        read, module, scanAnchor, relativeEntry, sink));
    return result;
}

CoordinateExecutionCandidateScanResult
DiscoverCoordinateExecutionCandidates(
    const CoordinateExecutionReadCallback& read,
    const CoordinateExecutionModuleSnapshot& module,
    std::uint64_t configuredModuleBase,
    std::uint32_t scanProfile) {
    std::uint64_t scanAnchor = 0;
    std::uint64_t relativeEntry = 0;
    if (!ResolveDiscoveryInput(
            read,
            module,
            configuredModuleBase,
            scanProfile,
            &scanAnchor,
            &relativeEntry)) {
        return {};
    }
    return ScanCoordinateExecutionCandidates(
        read, module, scanAnchor, relativeEntry);
}

bool ScanFirstCoordinateExecutionCandidate(
    const CoordinateExecutionReadCallback& read,
    const CoordinateExecutionModuleSnapshot& module,
    std::uint64_t scanAnchor,
    std::uint64_t relativeEntry,
    CoordinateExecutionCandidate* candidate) {
    if (candidate == nullptr) return false;
    *candidate = {};
    bool found = false;
    const CandidateSink sink = [&found, candidate](
                                   const CoordinateExecutionCandidate& value) {
        *candidate = value;
        found = true;
        return false;
    };
    static_cast<void>(ScanInPriorityOrder(
        read, module, scanAnchor, relativeEntry, sink));
    return found;
}

bool DiscoverFirstCoordinateExecutionCandidate(
    const CoordinateExecutionReadCallback& read,
    const CoordinateExecutionModuleSnapshot& module,
    std::uint64_t configuredModuleBase,
    std::uint32_t scanProfile,
    CoordinateExecutionCandidate* candidate) {
    if (candidate == nullptr) return false;
    *candidate = {};
    std::uint64_t scanAnchor = 0;
    std::uint64_t relativeEntry = 0;
    return ResolveDiscoveryInput(
               read,
               module,
               configuredModuleBase,
               scanProfile,
               &scanAnchor,
               &relativeEntry) &&
        ScanFirstCoordinateExecutionCandidate(
               read, module, scanAnchor, relativeEntry, candidate);
}

}  // namespace lengjing::game::native
