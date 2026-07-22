#pragma once

#include "game/native/AlgorithmCoordinateDiagnostics.h"
#include "game/native/CoordinatePoolPolicy.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

namespace lengjing::game::native {

enum class RuntimeCoordinateCodecStage : std::uint8_t {
    Idle,
    HookValidated,
    CodeValidated,
    ContextResolved,
    Ready,
    RingDecoded,
    ObjectDecoded = RingDecoded,
    Failed,
};

enum class RuntimeCoordinateCodecError : std::uint16_t {
    None = 0,
    InvalidInput,
    HookReadFailed,
    HookMismatch,
    LiteralInvalid,
    TrampolineReadFailed,
    TrampolineMismatch,
    TrampolineMappingMismatch,
    CallbackMappingMismatch,
    CallbackCodeReadFailed,
    CallbackFingerprintMismatch,
    ContextReadFailed,
    StateUnstable,
    StateInvalid,
    ConfigInvalid,
    ObjectInvalid,
    ObjectReadFailed,
    ObjectUnstable,
    OwnerMismatch,
    TableHeaderReadFailed,
    TableHeaderInvalid,
    IndexReadFailed,
    PhysicalIndexInvalid,
    RecordReadFailed,
    RecordUnstable,
    RecordNotFound,
    RecordInactive,
    RecordKeyMismatch,
    OutputInvalid,
    VerticalAdjustmentReadFailed,
    VerticalAdjustmentUnstable,
    VerticalAdjustmentInvalid,
};

struct RuntimeCoordinateCodecLayout {
    std::uintptr_t hookRva = 0x0E7FA4CCULL;
    std::uint64_t wrapperFingerprint = UINT64_C(0x5D79D891F15D0076);
    std::uint64_t mainFingerprint = UINT64_C(0x09632A7122906F3F);
    std::uint64_t stateFingerprint = UINT64_C(0x6E0E62461FA6483C);
    std::uint64_t codecFingerprint = UINT64_C(0xEA17CD7CD1229484);
    std::uint32_t divisor = 107;
    std::uint32_t mask = 0xFF07;
    std::uint32_t zBias = 807;
};

struct RuntimeCoordinateCodecDiagnostic {
    RuntimeCoordinateCodecStage stage = RuntimeCoordinateCodecStage::Idle;
    RuntimeCoordinateCodecError error = RuntimeCoordinateCodecError::None;
    std::uintptr_t hook = 0;
    std::uintptr_t trampoline = 0;
    std::uintptr_t callback = 0;
    std::uintptr_t context = 0;
    std::uintptr_t link = 0;
    std::uintptr_t slot0 = 0;
    std::uintptr_t slot1 = 0;
    std::uint64_t rawX0 = 0;
    std::uint64_t x1 = 0;
    std::uint64_t stateInput = 0;
    std::uintptr_t state = 0;
    std::uintptr_t config = 0;
    std::uintptr_t object = 0;
    std::uintptr_t owner = 0;
    std::uintptr_t indexArray = 0;
    std::uintptr_t records = 0;
    std::uintptr_t record = 0;
    std::uint64_t token = 0;
    std::uint32_t divisor = 0;
    std::uint32_t mask = 0;
    std::uint32_t zBias = 0;
    std::uint32_t ring = 0;
    std::uint32_t codecSeed = 0;
    std::uint32_t tableSeed = 0;
    std::uint32_t capacity = 0;
    std::uint32_t count = 0;
    std::uint32_t physicalIndex =
        std::numeric_limits<std::uint32_t>::max();
    std::uint32_t ringSlot = 0;
    std::uint64_t tableSalt = 0;
    std::uint64_t ringSalt = 0;
    std::uint64_t encodedFieldKey = 0;
    std::uint64_t encodedObjectKey = 0;
    std::uint64_t encodedOwnerKey = 0;
    std::uint8_t fingerprintWindow = 0;
    std::uint64_t expectedFingerprint = 0;
    std::uint64_t observedFingerprint = 0;
    float delta = 0.0f;
    float encodedX = 0.0f;
    float encodedY = 0.0f;
    float encodedZ = 0.0f;
    float decodedX = 0.0f;
    float decodedY = 0.0f;
    float decodedZ = 0.0f;
    float verticalAdjustmentFirst = 0.0f;
    float verticalAdjustmentSecond = 0.0f;
    float presentedZ = 0.0f;
};

constexpr std::uint16_t RuntimeCoordinateCodecErrorCode(
    RuntimeCoordinateCodecError error) noexcept {
    return static_cast<std::uint16_t>(error);
}

inline std::string FormatRuntimeCoordinateCodecDiagnostic(
    const RuntimeCoordinateCodecDiagnostic& diagnostic) {
    char buffer[24]{};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "ALGO RC-%04u",
        static_cast<unsigned>(RuntimeCoordinateCodecErrorCode(
            diagnostic.error)));
    return buffer;
}

class RuntimeCoordinateCodec final {
public:
    struct Coordinate {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    explicit RuntimeCoordinateCodec(
        RuntimeCoordinateCodecLayout layout = {}) noexcept
        : layout_(layout) {}

    template <typename ReadBytes,
              typename ValidateTrampoline,
              typename ValidateCallback>
    bool Refresh(std::uintptr_t moduleBase,
                 ReadBytes&& readBytes,
                 ValidateTrampoline&& validateTrampoline,
                 ValidateCallback&& validateCallback) noexcept {
        context_ = {};
        hookLiteral_ = 0;
        diagnostic_ = {};
        if (moduleBase == 0 || layout_.hookRva == 0) {
            return Fail(RuntimeCoordinateCodecError::InvalidInput);
        }
        if (!CheckedAdd(moduleBase, layout_.hookRva, diagnostic_.hook)) {
            return Fail(RuntimeCoordinateCodecError::InvalidInput);
        }

        std::array<std::uint32_t, 11> hook{};
        if (!readBytes(
                diagnostic_.hook, hook.data(), sizeof(hook))) {
            return Fail(RuntimeCoordinateCodecError::HookReadFailed);
        }
        for (std::size_t index = 0; index < 6; ++index) {
            if (hook[index] != UINT32_C(0xD503201F)) {
                return Fail(RuntimeCoordinateCodecError::HookMismatch);
            }
        }
        if (hook[6] != UINT32_C(0xBD016800) ||
            hook[7] != UINT32_C(0xBD016C01) ||
            hook[8] != UINT32_C(0xBD017002) ||
            hook[9] != UINT32_C(0x58000050) ||
            hook[10] != UINT32_C(0xD61F0200)) {
            return Fail(RuntimeCoordinateCodecError::HookMismatch);
        }
        diagnostic_.stage = RuntimeCoordinateCodecStage::HookValidated;

        std::uintptr_t literal = 0;
        if (!DecodeLdrLiteralAddress(
                hook[9], diagnostic_.hook + 9 * sizeof(std::uint32_t),
                literal)) {
            return Fail(RuntimeCoordinateCodecError::LiteralInvalid);
        }
        std::uint64_t trampolineRaw = 0;
        if (!ReadValue(readBytes, literal, trampolineRaw)) {
            return Fail(RuntimeCoordinateCodecError::TrampolineReadFailed);
        }
        diagnostic_.trampoline = StripPointer(trampolineRaw);
        if (diagnostic_.trampoline == 0 ||
            (diagnostic_.trampoline & 3U) != 0 ||
            !validateTrampoline(diagnostic_.trampoline)) {
            return Fail(
                RuntimeCoordinateCodecError::TrampolineMappingMismatch);
        }

        std::array<std::uint8_t, 0xA8> trampoline{};
        if (!readBytes(
                diagnostic_.trampoline,
                trampoline.data(),
                trampoline.size())) {
            return Fail(RuntimeCoordinateCodecError::TrampolineReadFailed);
        }
        if (Load32(trampoline.data()) != UINT32_C(0xA93E77FE) ||
            Load32(trampoline.data() + 4) != UINT32_C(0xA93D77FC) ||
            Load32(trampoline.data() + 0x8C) != UINT32_C(0x58FFFB60) ||
            Load32(trampoline.data() + 0x90) != UINT32_C(0x910003E1) ||
            Load32(trampoline.data() + 0x94) != UINT32_C(0x100000BE) ||
            Load32(trampoline.data() + 0x98) != UINT32_C(0x58000050) ||
            Load32(trampoline.data() + 0x9C) != UINT32_C(0xD61F0200)) {
            return Fail(RuntimeCoordinateCodecError::TrampolineMismatch);
        }
        const std::uint64_t callbackRaw =
            Load64(trampoline.data() + 0xA0);
        diagnostic_.callback = StripPointer(callbackRaw);
        if (diagnostic_.callback == 0 ||
            (diagnostic_.callback & 3U) != 0 ||
            !validateCallback(diagnostic_.callback)) {
            return Fail(
                RuntimeCoordinateCodecError::CallbackMappingMismatch);
        }

        if (!ValidateFingerprint(
                readBytes, 0, 0, 0xA0,
                layout_.wrapperFingerprint) ||
            !ValidateFingerprint(
                readBytes, 1, 0x26F4, 0x40,
                layout_.mainFingerprint) ||
            !ValidateFingerprint(
                readBytes, 2, 0xB7A4, 0x58,
                layout_.stateFingerprint) ||
            !ValidateFingerprint(
                readBytes, 3, 0x5324, 0x78,
                layout_.codecFingerprint)) {
            return false;
        }
        diagnostic_.stage = RuntimeCoordinateCodecStage::CodeValidated;

        if (diagnostic_.trampoline < sizeof(std::uint64_t)) {
            return Fail(RuntimeCoordinateCodecError::ContextReadFailed);
        }
        std::uint64_t contextRaw = 0;
        if (!ReadValue(
                readBytes,
                diagnostic_.trampoline - sizeof(std::uint64_t),
                contextRaw)) {
            return Fail(RuntimeCoordinateCodecError::ContextReadFailed);
        }
        const std::uintptr_t context = StripPointer(contextRaw);
        if (context == 0) {
            return Fail(RuntimeCoordinateCodecError::ContextReadFailed);
        }

        Context first{};
        Context second{};
        if (!ReadContext(context, first, readBytes) ||
            !ReadContext(context, second, readBytes)) {
            return false;
        }
        if (!SameContext(first, second)) {
            return Fail(RuntimeCoordinateCodecError::StateUnstable);
        }
        context_ = second;
        hookLiteral_ = literal;
        CopyContextToDiagnostic(context_);
        diagnostic_.stage = RuntimeCoordinateCodecStage::Ready;
        diagnostic_.error = RuntimeCoordinateCodecError::None;
        return true;
    }

    template <typename ReadBytes>
    bool Decode(std::uintptr_t object,
                std::uintptr_t expectedOwner,
                Coordinate& coordinate,
                RuntimeCoordinateCodecDiagnostic& diagnostic,
                ReadBytes&& readBytes) const noexcept {
        coordinate = {};
        diagnostic = diagnostic_;
        diagnostic.object = object;
        diagnostic.owner = expectedOwner;
        if (!Ready() || !IsPlainPointer(object) ||
            !IsPlainPointer(expectedOwner)) {
            return FailDecode(
                diagnostic, RuntimeCoordinateCodecError::ObjectInvalid);
        }

        constexpr unsigned kMaximumAttempts = 3;
        RuntimeCoordinateCodecError lastError =
            RuntimeCoordinateCodecError::RecordNotFound;
        for (unsigned attempt = 0; attempt < kMaximumAttempts; ++attempt) {
            if (!ValidateRuntimeEpoch(readBytes)) {
                lastError = RuntimeCoordinateCodecError::StateUnstable;
                continue;
            }
            TableHeader firstHeader{};
            if (!ReadTableHeader(
                    context_.state, firstHeader, readBytes)) {
                lastError =
                    RuntimeCoordinateCodecError::TableHeaderReadFailed;
                continue;
            }
            if (!ValidateTableHeader(firstHeader)) {
                return FailDecode(
                    diagnostic,
                    RuntimeCoordinateCodecError::TableHeaderInvalid);
            }
            CopyTableHeaderToDiagnostic(firstHeader, diagnostic);

            std::uint64_t firstOwner = 0;
            if (!ReadAt(readBytes, object, 0xE8, firstOwner)) {
                lastError = RuntimeCoordinateCodecError::ObjectReadFailed;
                continue;
            }
            if (StripPointer(firstOwner) != expectedOwner) {
                diagnostic.token = firstOwner;
                return FailDecode(
                    diagnostic, RuntimeCoordinateCodecError::OwnerMismatch);
            }

            std::uintptr_t fieldAddress = 0;
            if (!CheckedAdd(object, 0x168, fieldAddress)) {
                return FailDecode(
                    diagnostic, RuntimeCoordinateCodecError::ObjectInvalid);
            }
            const std::uint64_t fieldKey = EncodeRecordValue(
                static_cast<std::uint64_t>(fieldAddress),
                firstHeader.codecSeed);
            const std::uint64_t objectKey = EncodeRecordValue(
                static_cast<std::uint64_t>(object),
                firstHeader.codecSeed);
            const std::uint64_t ownerKey = EncodeRecordValue(
                firstOwner, firstHeader.codecSeed);
            diagnostic.token = firstOwner;
            diagnostic.encodedFieldKey = fieldKey;
            diagnostic.encodedObjectKey = objectKey;
            diagnostic.encodedOwnerKey = ownerKey;

            std::uint32_t lower = 0;
            std::uint32_t upper = firstHeader.count;
            bool retry = false;
            while (lower < upper) {
                const std::uint32_t middle = lower + (upper - lower) / 2U;
                SortedEntry entry{};
                if (!ReadStableSortedEntry(
                        firstHeader, middle, entry, readBytes)) {
                    lastError = RuntimeCoordinateCodecError::IndexReadFailed;
                    retry = true;
                    break;
                }
                if (entry.physicalIndex >= firstHeader.capacity) {
                    lastError =
                        RuntimeCoordinateCodecError::PhysicalIndexInvalid;
                    retry = true;
                    break;
                }
                if (entry.fieldKey < fieldKey) {
                    lower = middle + 1U;
                } else {
                    upper = middle;
                }
            }
            if (retry) continue;

            bool matchingFieldSeen = false;
            bool inactiveMatchSeen = false;
            bool keyMismatchSeen = false;
            for (std::uint32_t sorted = lower;
                 sorted < firstHeader.count;
                 ++sorted) {
                SortedEntry entry{};
                if (!ReadStableSortedEntry(
                        firstHeader, sorted, entry, readBytes)) {
                    lastError = RuntimeCoordinateCodecError::IndexReadFailed;
                    retry = true;
                    break;
                }
                if (entry.physicalIndex >= firstHeader.capacity) {
                    lastError =
                        RuntimeCoordinateCodecError::PhysicalIndexInvalid;
                    retry = true;
                    break;
                }
                if (entry.fieldKey != fieldKey) break;
                matchingFieldSeen = true;

                std::uintptr_t recordAddress = 0;
                if (!RecordAddress(
                        firstHeader.records,
                        entry.physicalIndex,
                        recordAddress)) {
                    return FailDecode(
                        diagnostic,
                        RuntimeCoordinateCodecError::PhysicalIndexInvalid);
                }
                RecordBytes firstRecord{};
                RecordBytes secondRecord{};
                if (!readBytes(
                        recordAddress,
                        firstRecord.data(),
                        firstRecord.size()) ||
                    !readBytes(
                        recordAddress,
                        secondRecord.data(),
                        secondRecord.size())) {
                    lastError = RuntimeCoordinateCodecError::RecordReadFailed;
                    retry = true;
                    break;
                }
                if (firstRecord != secondRecord) {
                    lastError = RuntimeCoordinateCodecError::RecordUnstable;
                    retry = true;
                    break;
                }

                const std::uint64_t observedFieldKey =
                    Load64(secondRecord.data() + kFieldKeyOffset);
                const std::uint64_t observedObjectKey =
                    Load64(secondRecord.data() + kObjectKeyOffset);
                const std::uint64_t observedOwnerKey =
                    Load64(secondRecord.data() + kOwnerKeyOffset);
                if (observedFieldKey != fieldKey ||
                    observedObjectKey != objectKey ||
                    observedOwnerKey != ownerKey) {
                    keyMismatchSeen = true;
                    continue;
                }
                if (secondRecord[kActiveFlagOffset] != 0) {
                    inactiveMatchSeen = true;
                    continue;
                }

                const std::uint64_t encodedSalt =
                    Load64(secondRecord.data() + kRingSaltOffset);
                const std::uint64_t ringSalt = DecodeRecordValue(
                    encodedSalt, firstHeader.codecSeed);
                const std::uint32_t ringSlot =
                    static_cast<std::uint32_t>(
                        (static_cast<std::uint64_t>(
                             firstHeader.globalRing) +
                         ringSalt) %
                        kRingSlotCount);
                const std::size_t coordinateOffset =
                    kRingOffset +
                    static_cast<std::size_t>(ringSlot) *
                        kRingSlotStride;
                Coordinate candidate{};
                std::memcpy(
                    &candidate,
                    secondRecord.data() + coordinateOffset,
                    sizeof(candidate));

                std::uint64_t secondOwner = 0;
                TableHeader secondHeader{};
                if (!ReadAt(readBytes, object, 0xE8, secondOwner) ||
                    !ReadTableHeader(
                        context_.state, secondHeader, readBytes)) {
                    lastError = RuntimeCoordinateCodecError::StateUnstable;
                    retry = true;
                    break;
                }
                if (firstOwner != secondOwner ||
                    !SameTableHeader(firstHeader, secondHeader)) {
                    lastError = RuntimeCoordinateCodecError::StateUnstable;
                    retry = true;
                    break;
                }
                if (!ValidateRuntimeEpoch(readBytes)) {
                    lastError = RuntimeCoordinateCodecError::StateUnstable;
                    retry = true;
                    break;
                }
                SortedEntry finalEntry{};
                if (!ReadStableSortedEntry(
                        firstHeader, sorted, finalEntry, readBytes)) {
                    lastError = RuntimeCoordinateCodecError::IndexReadFailed;
                    retry = true;
                    break;
                }
                if (finalEntry.physicalIndex != entry.physicalIndex ||
                    finalEntry.fieldKey != entry.fieldKey) {
                    lastError = RuntimeCoordinateCodecError::StateUnstable;
                    retry = true;
                    break;
                }
                if (!IsValidAlgorithmCoordinateValue(
                        candidate.x, candidate.y, candidate.z)) {
                    return FailDecode(
                        diagnostic,
                        RuntimeCoordinateCodecError::OutputInvalid);
                }

                coordinate = candidate;
                diagnostic.record = recordAddress;
                diagnostic.physicalIndex = entry.physicalIndex;
                diagnostic.ringSalt = ringSalt;
                diagnostic.ringSlot = ringSlot;
                diagnostic.decodedX = candidate.x;
                diagnostic.decodedY = candidate.y;
                diagnostic.decodedZ = candidate.z;
                diagnostic.stage = RuntimeCoordinateCodecStage::RingDecoded;
                diagnostic.error = RuntimeCoordinateCodecError::None;
                return true;
            }
            if (retry) continue;

            std::uint64_t secondOwner = 0;
            TableHeader secondHeader{};
            if (!ReadAt(readBytes, object, 0xE8, secondOwner) ||
                !ReadTableHeader(
                    context_.state, secondHeader, readBytes) ||
                firstOwner != secondOwner ||
                !SameTableHeader(firstHeader, secondHeader)) {
                lastError = RuntimeCoordinateCodecError::StateUnstable;
                continue;
            }
            if (!ValidateRuntimeEpoch(readBytes)) {
                lastError = RuntimeCoordinateCodecError::StateUnstable;
                continue;
            }
            if (inactiveMatchSeen) {
                return FailDecode(
                    diagnostic, RuntimeCoordinateCodecError::RecordInactive);
            }
            if (matchingFieldSeen && keyMismatchSeen) {
                return FailDecode(
                    diagnostic,
                    RuntimeCoordinateCodecError::RecordKeyMismatch);
            }
            return FailDecode(
                diagnostic, RuntimeCoordinateCodecError::RecordNotFound);
        }
        return FailDecode(diagnostic, lastError);
    }

    bool Ready() const noexcept {
        return diagnostic_.stage == RuntimeCoordinateCodecStage::Ready &&
            diagnostic_.error == RuntimeCoordinateCodecError::None;
    }

    RuntimeCoordinateCodecDiagnostic Diagnostic() const noexcept {
        return diagnostic_;
    }

    void Reset() noexcept {
        context_ = {};
        hookLiteral_ = 0;
        diagnostic_ = {};
    }

    static constexpr std::uintptr_t StripPointer(
        std::uint64_t value) noexcept {
        return static_cast<std::uintptr_t>(
            value & UINT64_C(0x00FFFFFFFFFFFFFF));
    }

    static constexpr std::uint64_t DecodeStateAddress(
        std::uint64_t rawX0,
        std::uint64_t stateInput) noexcept {
        const std::uint64_t mixed = rawX0 ^ UINT64_C(0x454E8BC8);
        const std::uint64_t value = stateInput +
            UINT64_C(0x83738835CDD096E0) * mixed * mixed;
        return (~(value | UINT64_C(0x21D9E67BDC387420)) *
                   (value & UINT64_C(0x21D9E67BDC387420))) +
            ((value | UINT64_C(0xDE26198423C78BDF)) *
                (value & UINT64_C(0xDE26198423C78BDF))) +
            UINT64_C(0xCD758D6265F1E54F);
    }

    static constexpr std::uint64_t EncodeRecordValue(
        std::uint64_t plain,
        std::uint32_t seed) noexcept {
        const std::uint64_t wideSeed = seed;
        return UINT64_C(0x55855E7F0EEBF01F) * plain -
            UINT64_C(0x02EDF245F0C9D491) -
            UINT64_C(0x83738835CDD096E0) * wideSeed * wideSeed;
    }

    static constexpr std::uint64_t DecodeRecordValue(
        std::uint64_t encoded,
        std::uint32_t seed) noexcept {
        const std::uint64_t wideSeed = seed;
        return UINT64_C(0xDE26198423C78BDF) *
                (encoded + UINT64_C(0x83738835CDD096E0) *
                    wideSeed * wideSeed) -
            UINT64_C(0x328A729D9A0E1AB1);
    }

    static bool DecodeLdrLiteralAddress(
        std::uint32_t instruction,
        std::uintptr_t pc,
        std::uintptr_t& address) noexcept {
        address = 0;
        if ((instruction & UINT32_C(0xFF000000)) !=
            UINT32_C(0x58000000)) {
            return false;
        }
        std::int64_t immediate = static_cast<std::int64_t>(
            (instruction >> 5U) & UINT32_C(0x7FFFF));
        if ((immediate & (INT64_C(1) << 18)) != 0) {
            immediate -= INT64_C(1) << 19;
        }
        immediate *= 4;
        if (immediate >= 0) {
            const auto offset = static_cast<std::uintptr_t>(immediate);
            if (offset > std::numeric_limits<std::uintptr_t>::max() - pc) {
                return false;
            }
            address = pc + offset;
            return true;
        }
        const auto offset = static_cast<std::uintptr_t>(-immediate);
        if (offset > pc) return false;
        address = pc - offset;
        return true;
    }

private:
    static constexpr std::size_t kRecordSize = 0x568;
    static constexpr std::size_t kFieldKeyOffset = 0x10;
    static constexpr std::size_t kRingOffset = 0x18;
    static constexpr std::size_t kRingSlotStride = 0x0C;
    static constexpr std::uint64_t kRingSlotCount = 14;
    static constexpr std::size_t kObjectKeyOffset = 0x530;
    static constexpr std::size_t kActiveFlagOffset = 0x53D;
    static constexpr std::size_t kRingSaltOffset = 0x540;
    static constexpr std::size_t kOwnerKeyOffset = 0x548;
    static constexpr std::uint32_t kMaximumRecordCapacity = 16384;

    using RecordBytes = std::array<std::uint8_t, kRecordSize>;

    struct TableHeader {
        std::uintptr_t indexArray = 0;
        std::uintptr_t auxiliary = 0;
        std::uintptr_t records = 0;
        std::uint32_t codecSeed = 0;
        std::uint32_t capacity = 0;
        std::uint32_t count = 0;
        std::uint32_t tableSeed = 0;
        std::uint32_t globalRing = 0;
        std::uint64_t tableSalt = 0;
    };

    struct Context {
        std::uintptr_t context = 0;
        std::uintptr_t link = 0;
        std::uintptr_t slot0 = 0;
        std::uintptr_t slot1 = 0;
        std::uint64_t rawX0 = 0;
        std::uint64_t x1 = 0;
        std::uint64_t stateInput = 0;
        std::uintptr_t state = 0;
        std::uintptr_t config = 0;
        std::uint32_t divisor = 0;
        std::uint32_t mask = 0;
        std::uint32_t zBias = 0;
        TableHeader table{};
    };

    struct SortedEntry {
        std::uint32_t physicalIndex = 0;
        std::uint64_t fieldKey = 0;
    };

    bool Fail(RuntimeCoordinateCodecError error) noexcept {
        context_ = {};
        hookLiteral_ = 0;
        diagnostic_.stage = RuntimeCoordinateCodecStage::Failed;
        diagnostic_.error = error;
        return false;
    }

    static bool FailDecode(
        RuntimeCoordinateCodecDiagnostic& diagnostic,
        RuntimeCoordinateCodecError error) noexcept {
        diagnostic.stage = RuntimeCoordinateCodecStage::Failed;
        diagnostic.error = error;
        return false;
    }

    template <typename ReadBytes>
    bool ValidateFingerprint(ReadBytes& readBytes,
                             std::uint8_t window,
                             std::uintptr_t offset,
                             std::size_t size,
                             std::uint64_t expected) noexcept {
        std::array<std::uint8_t, 0xA0> bytes{};
        if (size > bytes.size()) {
            return Fail(RuntimeCoordinateCodecError::InvalidInput);
        }
        std::uintptr_t address = 0;
        if (!CheckedAdd(diagnostic_.callback, offset, address) ||
            !readBytes(address, bytes.data(), size)) {
            diagnostic_.fingerprintWindow = window;
            return Fail(
                RuntimeCoordinateCodecError::CallbackCodeReadFailed);
        }
        const std::uint64_t observed =
            CoordinatePoolCodeFingerprint(bytes.data(), size);
        if (observed != expected) {
            diagnostic_.fingerprintWindow = window;
            diagnostic_.expectedFingerprint = expected;
            diagnostic_.observedFingerprint = observed;
            return Fail(
                RuntimeCoordinateCodecError::CallbackFingerprintMismatch);
        }
        return true;
    }

    template <typename ReadBytes>
    bool ReadContext(std::uintptr_t context,
                     Context& result,
                     ReadBytes& readBytes) noexcept {
        result = {};
        result.context = context;
        std::uint64_t raw = 0;
        if (!ReadAt(readBytes, context, 0x10, raw)) {
            return Fail(RuntimeCoordinateCodecError::ContextReadFailed);
        }
        result.link = StripPointer(raw);
        if (result.link == 0 ||
            !ReadAt(readBytes, result.link, 0x10, raw)) {
            return Fail(RuntimeCoordinateCodecError::ContextReadFailed);
        }
        result.slot0 = StripPointer(raw);
        if (!ReadAt(readBytes, result.link, 0x28, raw)) {
            return Fail(RuntimeCoordinateCodecError::ContextReadFailed);
        }
        result.slot1 = StripPointer(raw);
        if (result.slot0 == 0 || result.slot1 == 0 ||
            !ReadValue(readBytes, result.slot0, result.rawX0) ||
            !ReadValue(readBytes, result.slot1, result.x1) ||
            result.x1 != 1) {
            return Fail(RuntimeCoordinateCodecError::ContextReadFailed);
        }
        const std::uintptr_t x0Address = StripPointer(result.rawX0);
        if (x0Address == 0 ||
            !ReadAt(readBytes, x0Address, 0x30, result.stateInput)) {
            return Fail(RuntimeCoordinateCodecError::ContextReadFailed);
        }

        const std::uint64_t stateRaw = DecodeStateAddress(
            result.rawX0, result.stateInput);
        if ((stateRaw & ~UINT64_C(0x00FFFFFFFFFFFFFF)) != 0 ||
            (stateRaw & UINT64_C(0xFFF)) != 0) {
            return Fail(RuntimeCoordinateCodecError::StateInvalid);
        }
        result.state = static_cast<std::uintptr_t>(stateRaw);
        if (result.state == 0 ||
            !ReadAt(readBytes, result.state, 0x2740, raw)) {
            return Fail(RuntimeCoordinateCodecError::StateInvalid);
        }
        result.config = StripPointer(raw);
        std::array<std::uint32_t, 4> parameters{};
        if (result.config == 0 ||
            !ReadAt(
                readBytes, result.config, 0x210,
                parameters)) {
            return Fail(RuntimeCoordinateCodecError::ConfigInvalid);
        }
        result.divisor = parameters[0];
        result.mask = parameters[1];
        result.zBias = parameters[3];
        if (result.divisor != layout_.divisor ||
            result.mask != layout_.mask ||
            result.zBias != layout_.zBias) {
            return Fail(RuntimeCoordinateCodecError::ConfigInvalid);
        }
        if (!ReadTableHeader(result.state, result.table, readBytes)) {
            return Fail(
                RuntimeCoordinateCodecError::TableHeaderReadFailed);
        }
        if (!ValidateTableHeader(result.table)) {
            return Fail(RuntimeCoordinateCodecError::TableHeaderInvalid);
        }
        return true;
    }

    template <typename ReadBytes>
    static bool ReadTableHeader(std::uintptr_t state,
                                TableHeader& header,
                                ReadBytes& readBytes) noexcept {
        header = {};
        std::uint64_t rawIndexArray = 0;
        std::uint64_t rawAuxiliary = 0;
        if (!ReadAt(readBytes, state, 0x10, rawIndexArray) ||
            !ReadAt(readBytes, state, 0xAC0, rawAuxiliary) ||
            !ReadAt(readBytes, state, 0xC20, header.codecSeed) ||
            !ReadAt(readBytes, state, 0xC40, header.capacity) ||
            !ReadAt(readBytes, state, 0x14A0, header.count) ||
            !ReadAt(readBytes, state, 0x186C, header.tableSeed) ||
            !ReadAt(readBytes, state, 0x2118, header.globalRing) ||
            !ReadAt(readBytes, state, 0x2598, header.tableSalt)) {
            return false;
        }
        header.indexArray = StripPointer(rawIndexArray);
        header.auxiliary = StripPointer(rawAuxiliary);
        const std::uint32_t recordsSeed = header.tableSeed + 2U;
        const std::uint64_t recordsRaw = DecodeRecordValue(
            header.tableSalt, recordsSeed);
        if (recordsRaw >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::uintptr_t>::max())) {
            return false;
        }
        header.records = static_cast<std::uintptr_t>(recordsRaw);
        return true;
    }

    static bool ValidateTableHeader(const TableHeader& header) noexcept {
        return IsPlainPointer(header.indexArray) &&
            IsPlainPointer(header.records) &&
            (header.indexArray & 3U) == 0 &&
            (header.records & 7U) == 0 && header.count != 0 &&
            header.capacity != 0 &&
            header.capacity <= kMaximumRecordCapacity &&
            header.count <= header.capacity &&
            header.globalRing < kRingSlotCount;
    }

    static bool SameTableHeader(const TableHeader& left,
                                const TableHeader& right) noexcept {
        return left.indexArray == right.indexArray &&
            left.auxiliary == right.auxiliary &&
            left.records == right.records &&
            left.codecSeed == right.codecSeed &&
            left.capacity == right.capacity && left.count == right.count &&
            left.tableSeed == right.tableSeed &&
            left.globalRing == right.globalRing &&
            left.tableSalt == right.tableSalt;
    }

    template <typename ReadBytes>
    static bool ReadStableSortedEntry(const TableHeader& header,
                                      std::uint32_t sortedIndex,
                                      SortedEntry& entry,
                                      ReadBytes& readBytes) noexcept {
        entry = {};
        std::uintptr_t indexAddress = 0;
        std::uintptr_t indexOffset = 0;
        if (!CheckedMultiply(
                sortedIndex,
                sizeof(std::uint32_t),
                indexOffset) ||
            !CheckedAdd(header.indexArray, indexOffset, indexAddress)) {
            return false;
        }
        std::int32_t firstPhysical = 0;
        std::int32_t secondPhysical = 0;
        if (!ReadValue(readBytes, indexAddress, firstPhysical) ||
            !ReadValue(readBytes, indexAddress, secondPhysical) ||
            firstPhysical != secondPhysical) {
            return false;
        }
        if (secondPhysical < 0) {
            entry.physicalIndex =
                std::numeric_limits<std::uint32_t>::max();
            return true;
        }
        entry.physicalIndex = static_cast<std::uint32_t>(secondPhysical);
        if (entry.physicalIndex >= header.capacity) return true;

        std::uintptr_t recordAddress = 0;
        if (!RecordAddress(
                header.records,
                entry.physicalIndex,
                recordAddress)) {
            return false;
        }
        std::uint64_t firstKey = 0;
        std::uint64_t secondKey = 0;
        if (!ReadAt(
                readBytes,
                recordAddress,
                kFieldKeyOffset,
                firstKey) ||
            !ReadAt(
                readBytes,
                recordAddress,
                kFieldKeyOffset,
                secondKey) ||
            firstKey != secondKey) {
            return false;
        }
        entry.fieldKey = secondKey;
        return true;
    }

    template <typename ReadBytes>
    bool ValidateRuntimeEpoch(ReadBytes& readBytes) const noexcept {
        if (hookLiteral_ == 0 || context_.context == 0 ||
            diagnostic_.trampoline < sizeof(std::uint64_t)) {
            return false;
        }

        std::uint64_t raw = 0;
        if (!ReadValue(readBytes, hookLiteral_, raw) ||
            StripPointer(raw) != diagnostic_.trampoline ||
            !ReadValue(
                readBytes,
                diagnostic_.trampoline - sizeof(std::uint64_t),
                raw) ||
            StripPointer(raw) != context_.context ||
            !ReadAt(readBytes, context_.context, 0x10, raw) ||
            StripPointer(raw) != context_.link ||
            !ReadAt(readBytes, context_.link, 0x10, raw) ||
            StripPointer(raw) != context_.slot0 ||
            !ReadAt(readBytes, context_.link, 0x28, raw) ||
            StripPointer(raw) != context_.slot1) {
            return false;
        }

        std::uint64_t rawX0 = 0;
        std::uint64_t x1 = 0;
        std::uint64_t stateInput = 0;
        const std::uintptr_t x0Address = StripPointer(context_.rawX0);
        if (!ReadValue(readBytes, context_.slot0, rawX0) ||
            rawX0 != context_.rawX0 ||
            !ReadValue(readBytes, context_.slot1, x1) ||
            x1 != context_.x1 || x0Address == 0 ||
            !ReadAt(readBytes, x0Address, 0x30, stateInput) ||
            stateInput != context_.stateInput ||
            DecodeStateAddress(rawX0, stateInput) != context_.state) {
            return false;
        }
        return true;
    }

    static bool RecordAddress(std::uintptr_t records,
                              std::uint32_t physicalIndex,
                              std::uintptr_t& address) noexcept {
        std::uintptr_t offset = 0;
        return CheckedMultiply(
                physicalIndex,
                kRecordSize,
                offset) &&
            CheckedAdd(records, offset, address);
    }

    static bool SameContext(const Context& left,
                            const Context& right) noexcept {
        return left.context == right.context &&
            left.link == right.link && left.slot0 == right.slot0 &&
            left.slot1 == right.slot1 && left.rawX0 == right.rawX0 &&
            left.x1 == right.x1 && left.stateInput == right.stateInput &&
            left.state == right.state && left.config == right.config &&
            left.divisor == right.divisor && left.mask == right.mask &&
            left.zBias == right.zBias &&
            SameTableHeader(left.table, right.table);
    }

    void CopyContextToDiagnostic(const Context& context) noexcept {
        diagnostic_.context = context.context;
        diagnostic_.link = context.link;
        diagnostic_.slot0 = context.slot0;
        diagnostic_.slot1 = context.slot1;
        diagnostic_.rawX0 = context.rawX0;
        diagnostic_.x1 = context.x1;
        diagnostic_.stateInput = context.stateInput;
        diagnostic_.state = context.state;
        diagnostic_.config = context.config;
        diagnostic_.divisor = context.divisor;
        diagnostic_.mask = context.mask;
        diagnostic_.zBias = context.zBias;
        CopyTableHeaderToDiagnostic(context.table, diagnostic_);
    }

    static void CopyTableHeaderToDiagnostic(
        const TableHeader& header,
        RuntimeCoordinateCodecDiagnostic& diagnostic) noexcept {
        diagnostic.indexArray = header.indexArray;
        diagnostic.records = header.records;
        diagnostic.codecSeed = header.codecSeed;
        diagnostic.capacity = header.capacity;
        diagnostic.count = header.count;
        diagnostic.tableSeed = header.tableSeed;
        diagnostic.ring = header.globalRing;
        diagnostic.tableSalt = header.tableSalt;
    }

    static std::uint32_t Load32(const std::uint8_t* bytes) noexcept {
        std::uint32_t value = 0;
        std::memcpy(&value, bytes, sizeof(value));
        return value;
    }

    static std::uint64_t Load64(const std::uint8_t* bytes) noexcept {
        std::uint64_t value = 0;
        std::memcpy(&value, bytes, sizeof(value));
        return value;
    }

    static bool CheckedAdd(std::uintptr_t base,
                           std::uintptr_t offset,
                           std::uintptr_t& result) noexcept {
        if (offset > std::numeric_limits<std::uintptr_t>::max() - base) {
            result = 0;
            return false;
        }
        result = base + offset;
        return true;
    }

    static bool CheckedMultiply(std::uintptr_t left,
                                std::uintptr_t right,
                                std::uintptr_t& result) noexcept {
        if (left != 0 &&
            right > std::numeric_limits<std::uintptr_t>::max() / left) {
            result = 0;
            return false;
        }
        result = left * right;
        return true;
    }

    static constexpr bool IsPlainPointer(std::uintptr_t value) noexcept {
        return value != 0 &&
            (static_cast<std::uint64_t>(value) &
                ~UINT64_C(0x00FFFFFFFFFFFFFF)) == 0;
    }

    template <typename ReadBytes, typename T>
    static bool ReadValue(ReadBytes& readBytes,
                          std::uintptr_t address,
                          T& value) noexcept {
        value = T{};
        return address != 0 &&
            static_cast<bool>(readBytes(address, &value, sizeof(value)));
    }

    template <typename ReadBytes, typename T>
    static bool ReadAt(ReadBytes& readBytes,
                       std::uintptr_t base,
                       std::uintptr_t offset,
                       T& value) noexcept {
        std::uintptr_t address = 0;
        return CheckedAdd(base, offset, address) &&
            ReadValue(readBytes, address, value);
    }

    RuntimeCoordinateCodecLayout layout_{};
    Context context_{};
    std::uintptr_t hookLiteral_ = 0;
    RuntimeCoordinateCodecDiagnostic diagnostic_{};
};

}  // namespace lengjing::game::native
