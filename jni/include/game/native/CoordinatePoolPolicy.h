#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace lengjing::game::native {

inline constexpr std::size_t kCoordinatePoolRingSearchesPerFrame = 4;
inline constexpr std::uint64_t kCoordinatePoolRingRetryFrames = 8;
inline constexpr std::uint8_t kCoordinatePoolRingReadFailureThreshold = 2;
inline constexpr std::uint64_t kCoordinatePoolCodeValidationRetryFrames = 8;
inline constexpr std::uint64_t kCoordinatePoolCodeValidationIdleFrame =
    UINT64_MAX;
inline constexpr std::uint64_t kCoordinatePoolPointerPayloadMask =
    UINT64_C(0x0000FFFFFFFFFFFF);
inline constexpr std::uint64_t kCoordinatePoolIndexedPointerPayloadMask =
    UINT64_C(0x00FFFFFFFFFFFFFF);
inline constexpr std::uint64_t kCoordinatePoolMinimumRemoteAddress =
    UINT64_C(0x10000000);
inline constexpr std::uint64_t kCoordinatePoolMaximumRemoteAddress =
    UINT64_C(0x10000000000);
inline constexpr std::size_t kCoordinatePoolBlockProbeCount = 20;
inline constexpr std::size_t kCoordinatePoolMaximumBlockCount =
    kCoordinatePoolBlockProbeCount - 1;
inline constexpr std::uint32_t kCoordinatePoolMaximumDecryptIndexOffset = 15;
inline constexpr std::uint8_t kCoordinatePoolUnknownDecryptIndexOffset =
    UINT8_MAX;
inline constexpr std::size_t kCoordinatePoolDecryptIndexEvidenceLimit = 64;
inline constexpr std::size_t kCoordinatePoolDecryptIndexMinimumEvidence = 3;
inline constexpr std::size_t kCoordinatePoolDecryptIndexMinimumComponents = 2;
inline constexpr std::size_t
    kCoordinatePoolDecryptIndexSingleComponentEvidence = 6;
inline constexpr std::size_t kCoordinatePoolDecryptIndexMinimumLead = 2;
inline constexpr std::size_t
    kCoordinatePoolDecryptIndexCalibrationReadsPerFrame = 4;
inline constexpr std::size_t
    kCoordinatePoolDecryptIndexAuditReadsPerFrame = 2;
inline constexpr std::uint64_t
    kCoordinatePoolDecryptIndexWitnessRefreshFrames = 30;
inline constexpr std::size_t
    kCoordinatePoolDecryptIndexContradictionEvidence = 3;
inline constexpr std::size_t
    kCoordinatePoolDecryptIndexSingleComponentContradictionEvidence = 6;
inline constexpr std::uint64_t
    kCoordinatePoolDecryptIndexFlickerGraceFrames = 12;
inline constexpr std::uint64_t
    kCoordinatePoolDecryptIndexFlickerWindowFrames = 24;
inline constexpr std::size_t
    kCoordinatePoolDecryptIndexFlickerEvidenceLimit = 32;
inline constexpr std::size_t
    kCoordinatePoolDecryptIndexFlickerMinimumEvidence = 4;
inline constexpr std::size_t
    kCoordinatePoolDecryptIndexFlickerMinimumComponents = 3;
inline constexpr std::size_t
    kCoordinatePoolDecryptIndexFlickerMinimumFrames = 2;

inline std::size_t ExpireCoordinatePoolDecryptIndexWitnesses(
    std::array<std::uint64_t,
               kCoordinatePoolDecryptIndexCalibrationReadsPerFrame>&
        witnesses,
    std::array<std::uint64_t,
               kCoordinatePoolDecryptIndexCalibrationReadsPerFrame>&
        lastSeenFrames,
    std::size_t witnessCount,
    std::uint64_t frame) noexcept {
    const std::size_t boundedCount =
        witnessCount < witnesses.size() ? witnessCount : witnesses.size();
    std::size_t retainedCount = 0;
    for (std::size_t index = 0; index < boundedCount; ++index) {
        const std::uint64_t lastSeen = lastSeenFrames[index];
        if (frame < lastSeen ||
            frame - lastSeen >=
                kCoordinatePoolDecryptIndexWitnessRefreshFrames) {
            continue;
        }
        witnesses[retainedCount] = witnesses[index];
        lastSeenFrames[retainedCount] = lastSeen;
        ++retainedCount;
    }
    for (std::size_t index = retainedCount;
         index < witnesses.size();
         ++index) {
        witnesses[index] = 0;
        lastSeenFrames[index] = 0;
    }
    return retainedCount;
}

inline constexpr std::size_t kCoordinatePoolCompactLogicalSlotCount = 5;
inline constexpr std::size_t kCoordinatePoolExtendedLogicalSlotCount = 7;
inline constexpr std::size_t kCoordinatePoolBankCount = 2;
inline constexpr std::size_t kCoordinatePoolLogicalCandidateCount =
    kCoordinatePoolExtendedLogicalSlotCount;
inline constexpr std::size_t kCoordinatePoolPhysicalSlotCount =
    kCoordinatePoolLogicalCandidateCount * kCoordinatePoolBankCount;
inline constexpr std::size_t kCoordinatePoolLayoutEvidenceLimit = 64;
inline constexpr std::size_t kCoordinatePoolLayoutMinimumEvidence = 3;
inline constexpr std::size_t kCoordinatePoolLayoutMinimumComponents = 2;
inline constexpr std::size_t
    kCoordinatePoolLayoutSingleComponentEvidence = 6;
inline constexpr std::size_t kCoordinatePoolLayoutMinimumLead = 2;
inline constexpr std::size_t
    kCoordinatePoolLayoutInvalidDecodedSlotLimit = 3;

enum class CoordinatePoolDecryptMode : std::uint8_t {
    None,
    Legacy,
    Indexed,
};

constexpr CoordinatePoolDecryptMode ResolveCoordinatePoolDecryptMode(
    bool legacyRequested,
    bool indexedRequested) noexcept {
    if (indexedRequested) return CoordinatePoolDecryptMode::Indexed;
    return legacyRequested
        ? CoordinatePoolDecryptMode::Legacy
        : CoordinatePoolDecryptMode::None;
}

constexpr bool IsCoordinatePoolDecryptRequested(
    CoordinatePoolDecryptMode mode) noexcept {
    return mode != CoordinatePoolDecryptMode::None;
}

constexpr bool IsCoordinatePoolIndexedDecrypt(
    CoordinatePoolDecryptMode mode) noexcept {
    return mode == CoordinatePoolDecryptMode::Indexed;
}

constexpr bool IsCoordinatePoolDecryptIndexOffsetValid(
    std::uint32_t offset) noexcept {
    return offset <= kCoordinatePoolMaximumDecryptIndexOffset;
}

constexpr std::size_t SelectCoordinatePoolIndexedSlot(
    std::uint64_t decodedSlot,
    std::uint32_t decryptIndexOffset,
    std::size_t blockCount) noexcept {
    if (!IsCoordinatePoolDecryptIndexOffsetValid(decryptIndexOffset) ||
        blockCount == 0 || blockCount > kCoordinatePoolMaximumBlockCount) {
        return kCoordinatePoolBlockProbeCount;
    }
    const std::uint64_t adjusted =
        decodedSlot + static_cast<std::uint64_t>(decryptIndexOffset);
    return static_cast<std::size_t>(adjusted % blockCount);
}

constexpr bool InferCoordinatePoolDecryptIndexOffset(
    std::uint64_t decodedSlot,
    std::size_t changedSlot,
    std::size_t blockCount,
    std::uint8_t& offset) noexcept {
    offset = kCoordinatePoolUnknownDecryptIndexOffset;
    if (blockCount == 0 ||
        blockCount > kCoordinatePoolMaximumBlockCount ||
        changedSlot >= blockCount) {
        return false;
    }
    for (std::uint8_t candidate = 0;
         candidate <= kCoordinatePoolMaximumDecryptIndexOffset;
         ++candidate) {
        if (SelectCoordinatePoolIndexedSlot(
                decodedSlot, candidate, blockCount) == changedSlot) {
            offset = candidate;
            return true;
        }
    }
    return false;
}

constexpr std::uint16_t MatchingCoordinatePoolDecryptIndexOffsets(
    std::uint64_t decodedSlot,
    std::uint32_t changedSlotMask,
    std::size_t blockCount) noexcept {
    if (blockCount == 0 ||
        blockCount > kCoordinatePoolMaximumBlockCount) {
        return 0;
    }
    std::uint16_t matches = 0;
    std::uint32_t seenSlots = 0;
    for (std::uint8_t offset = 0;
         offset <= kCoordinatePoolMaximumDecryptIndexOffset;
         ++offset) {
        const std::size_t slot = SelectCoordinatePoolIndexedSlot(
            decodedSlot, offset, blockCount);
        if (slot < blockCount &&
            (changedSlotMask & (UINT32_C(1) << slot)) != 0 &&
            (seenSlots & (UINT32_C(1) << slot)) == 0) {
            matches |= static_cast<std::uint16_t>(
                UINT16_C(1) << offset);
            seenSlots |= UINT32_C(1) << slot;
        }
    }
    return matches;
}

class CoordinatePoolDecryptIndexCalibration final {
public:
    void Observe(
        std::uintptr_t component,
        std::uint16_t matchingOffsets) noexcept {
        if (component == 0 || matchingOffsets == 0) return;
        if (locked_) {
            const std::uint16_t selectedBit = static_cast<std::uint16_t>(
                UINT16_C(1) << selected_);
            if (matchingOffsets == selectedBit) {
                contradictionCount_ = 0;
                contradictionComponent_ = 0;
                contradictionHasMultipleComponents_ = false;
                return;
            }
            if ((matchingOffsets & selectedBit) != 0) return;
            if (contradictionCount_ == 0) {
                contradictionComponent_ = component;
            } else if (component != contradictionComponent_) {
                contradictionHasMultipleComponents_ = true;
            }
            ++contradictionCount_;
            const std::size_t required =
                contradictionHasMultipleComponents_
                ? kCoordinatePoolDecryptIndexContradictionEvidence
                : kCoordinatePoolDecryptIndexSingleComponentContradictionEvidence;
            if (contradictionCount_ >= required) Reset();
            return;
        }
        evidence_[writeIndex_] = {component, matchingOffsets};
        writeIndex_ = (writeIndex_ + 1) % evidence_.size();
        if (evidenceCount_ < evidence_.size()) ++evidenceCount_;
        Resolve();
    }

    void Reset() noexcept { *this = {}; }

    std::uint8_t Resolve(std::uint8_t fallback) const noexcept {
        return locked_
            ? selected_
            : fallback;
    }

    bool IsLocked() const noexcept { return locked_; }
    std::uint8_t Selected() const noexcept { return selected_; }
    std::size_t Evidence() const noexcept { return bestEvidence_; }
    std::size_t ComponentCount() const noexcept { return componentCount_; }
    std::size_t Contradictions() const noexcept {
        return contradictionCount_;
    }

private:
    struct Observation {
        std::uintptr_t component = 0;
        std::uint16_t matchingOffsets = 0;
    };

    std::size_t CountComponents(std::uint8_t offset) const noexcept {
        std::array<std::uintptr_t,
                   kCoordinatePoolDecryptIndexEvidenceLimit> components{};
        std::size_t count = 0;
        const std::uint16_t offsetBit = static_cast<std::uint16_t>(
            UINT16_C(1) << offset);
        for (std::size_t index = 0; index < evidenceCount_; ++index) {
            if ((evidence_[index].matchingOffsets & offsetBit) == 0) continue;
            bool found = false;
            for (std::size_t componentIndex = 0;
                 componentIndex < count;
                 ++componentIndex) {
                if (components[componentIndex] ==
                    evidence_[index].component) {
                    found = true;
                    break;
                }
            }
            if (!found) components[count++] = evidence_[index].component;
        }
        return count;
    }

    void Resolve() noexcept {
        std::array<std::size_t,
                   kCoordinatePoolMaximumDecryptIndexOffset + 1> votes{};
        for (std::size_t index = 0; index < evidenceCount_; ++index) {
            for (std::uint8_t offset = 0;
                 offset <= kCoordinatePoolMaximumDecryptIndexOffset;
                 ++offset) {
                if ((evidence_[index].matchingOffsets &
                     (UINT16_C(1) << offset)) != 0) {
                    ++votes[offset];
                }
            }
        }

        std::uint16_t bestMask = 0;
        std::size_t bestEvidence = 0;
        std::size_t runnerUpEvidence = 0;
        for (std::uint8_t offset = 0;
             offset <= kCoordinatePoolMaximumDecryptIndexOffset;
             ++offset) {
            if (votes[offset] > bestEvidence) {
                runnerUpEvidence = bestEvidence;
                bestEvidence = votes[offset];
                bestMask = static_cast<std::uint16_t>(
                    UINT16_C(1) << offset);
            } else if (votes[offset] == bestEvidence) {
                bestMask |= static_cast<std::uint16_t>(
                    UINT16_C(1) << offset);
            } else if (votes[offset] > runnerUpEvidence) {
                runnerUpEvidence = votes[offset];
            }
        }
        if (bestEvidence == 0 || (bestMask & (bestMask - 1U)) != 0) return;

        std::uint8_t bestOffset = 0;
        while ((bestMask & (UINT16_C(1) << bestOffset)) == 0) ++bestOffset;
        const std::size_t componentCount = CountComponents(bestOffset);
        selected_ = bestOffset;
        bestEvidence_ = bestEvidence;
        componentCount_ = componentCount;
        const bool enoughComponents =
            componentCount >=
                kCoordinatePoolDecryptIndexMinimumComponents ||
            (componentCount == 1 &&
             bestEvidence >=
                 kCoordinatePoolDecryptIndexSingleComponentEvidence);
        if (bestEvidence < kCoordinatePoolDecryptIndexMinimumEvidence ||
            !enoughComponents ||
            bestEvidence <
                runnerUpEvidence +
                    kCoordinatePoolDecryptIndexMinimumLead) {
            return;
        }
        locked_ = true;
    }

    std::array<Observation,
               kCoordinatePoolDecryptIndexEvidenceLimit> evidence_{};
    std::size_t evidenceCount_ = 0;
    std::size_t writeIndex_ = 0;
    std::size_t bestEvidence_ = 0;
    std::size_t componentCount_ = 0;
    std::size_t contradictionCount_ = 0;
    std::uintptr_t contradictionComponent_ = 0;
    std::uint8_t selected_ = kCoordinatePoolUnknownDecryptIndexOffset;
    bool contradictionHasMultipleComponents_ = false;
    bool locked_ = false;
};

struct CoordinatePoolDecryptIndexSwitchDecision {
    bool requested = false;
    std::uint8_t currentOffset =
        kCoordinatePoolUnknownDecryptIndexOffset;
    std::uint8_t nextOffset =
        kCoordinatePoolUnknownDecryptIndexOffset;
    std::size_t evidence = 0;
    std::size_t components = 0;
    std::size_t frames = 0;
};

class CoordinatePoolDecryptIndexFlickerSwitch final {
public:
    CoordinatePoolDecryptIndexSwitchDecision Observe(
        std::uintptr_t component,
        std::uint32_t observedOffset,
        std::size_t blockCount,
        std::uint64_t frame,
        bool flicker) noexcept {
        if (component == 0) return {};
        if (!IsCoordinatePoolDecryptIndexOffsetValid(observedOffset) ||
            blockCount == 0 ||
            blockCount > kCoordinatePoolMaximumBlockCount) {
            Reset();
            return {};
        }

        const std::uint8_t candidateCount =
            static_cast<std::uint8_t>(
                blockCount <
                        kCoordinatePoolMaximumDecryptIndexOffset + 1
                    ? blockCount
                    : kCoordinatePoolMaximumDecryptIndexOffset + 1);
        if (candidateCount < 2) {
            Reset();
            return {};
        }
        const std::uint8_t normalizedOffset =
            static_cast<std::uint8_t>(observedOffset % candidateCount);
        if (!initialized_ || blockCount_ != blockCount ||
            frame < lastObservedFrame_ ||
            normalizedOffset != activeOffset_) {
            ResetForOffset(
                normalizedOffset, candidateCount, blockCount, frame);
        }
        lastObservedFrame_ = frame;
        Expire(frame);
        if (switchRequested_ ||
            frame - activatedFrame_ <
                kCoordinatePoolDecryptIndexFlickerGraceFrames ||
            !flicker) {
            return {};
        }
        for (std::size_t index = 0; index < observationCount_; ++index) {
            if (observations_[index].component == component &&
                observations_[index].frame == frame) {
                return {};
            }
        }
        if (observationCount_ == observations_.size()) {
            for (std::size_t index = 1;
                 index < observationCount_;
                 ++index) {
                observations_[index - 1] = observations_[index];
            }
            --observationCount_;
        }
        observations_[observationCount_++] = {component, frame};

        std::array<std::uintptr_t,
                   kCoordinatePoolDecryptIndexFlickerEvidenceLimit>
            components{};
        std::array<std::uint64_t,
                   kCoordinatePoolDecryptIndexFlickerEvidenceLimit>
            frames{};
        std::size_t componentCount = 0;
        std::size_t frameCount = 0;
        for (std::size_t index = 0; index < observationCount_; ++index) {
            const Observation& observation = observations_[index];
            bool componentSeen = false;
            for (std::size_t componentIndex = 0;
                 componentIndex < componentCount;
                 ++componentIndex) {
                if (components[componentIndex] ==
                    observation.component) {
                    componentSeen = true;
                    break;
                }
            }
            if (!componentSeen) {
                components[componentCount++] = observation.component;
            }

            bool frameSeen = false;
            for (std::size_t frameIndex = 0;
                 frameIndex < frameCount;
                 ++frameIndex) {
                if (frames[frameIndex] == observation.frame) {
                    frameSeen = true;
                    break;
                }
            }
            if (!frameSeen) frames[frameCount++] = observation.frame;
        }
        if (observationCount_ <
                kCoordinatePoolDecryptIndexFlickerMinimumEvidence ||
            componentCount <
                kCoordinatePoolDecryptIndexFlickerMinimumComponents ||
            frameCount < kCoordinatePoolDecryptIndexFlickerMinimumFrames) {
            return {};
        }

        CoordinatePoolDecryptIndexSwitchDecision decision{};
        decision.requested = true;
        decision.currentOffset = activeOffset_;
        decision.nextOffset = static_cast<std::uint8_t>(
            (activeOffset_ + 1) % candidateCount_);
        decision.evidence = observationCount_;
        decision.components = componentCount;
        decision.frames = frameCount;
        switchRequested_ = true;
        observations_ = {};
        observationCount_ = 0;
        return decision;
    }

    void Reset() noexcept { *this = {}; }

    std::uint8_t ActiveOffset() const noexcept {
        return initialized_
            ? activeOffset_
            : kCoordinatePoolUnknownDecryptIndexOffset;
    }

    std::size_t Evidence() const noexcept {
        return observationCount_;
    }

    bool SwitchRequested() const noexcept {
        return switchRequested_;
    }

private:
    struct Observation {
        std::uintptr_t component = 0;
        std::uint64_t frame = 0;
    };

    void ResetForOffset(
        std::uint8_t offset,
        std::uint8_t candidateCount,
        std::size_t blockCount,
        std::uint64_t frame) noexcept {
        observations_ = {};
        observationCount_ = 0;
        activatedFrame_ = frame;
        lastObservedFrame_ = frame;
        blockCount_ = blockCount;
        activeOffset_ = offset;
        candidateCount_ = candidateCount;
        switchRequested_ = false;
        initialized_ = true;
    }

    void Expire(std::uint64_t frame) noexcept {
        std::size_t retained = 0;
        for (std::size_t index = 0; index < observationCount_; ++index) {
            const Observation& observation = observations_[index];
            if (frame < observation.frame ||
                frame - observation.frame >=
                    kCoordinatePoolDecryptIndexFlickerWindowFrames) {
                continue;
            }
            observations_[retained++] = observation;
        }
        for (std::size_t index = retained;
             index < observationCount_;
             ++index) {
            observations_[index] = {};
        }
        observationCount_ = retained;
    }

    std::array<Observation,
               kCoordinatePoolDecryptIndexFlickerEvidenceLimit>
        observations_{};
    std::size_t observationCount_ = 0;
    std::uint64_t activatedFrame_ = 0;
    std::uint64_t lastObservedFrame_ = 0;
    std::size_t blockCount_ = 0;
    std::uint8_t activeOffset_ =
        kCoordinatePoolUnknownDecryptIndexOffset;
    std::uint8_t candidateCount_ = 0;
    bool switchRequested_ = false;
    bool initialized_ = false;
};

enum class CoordinatePoolSlotLayoutKind : std::uint8_t {
    Unknown,
    Compact,
    Extended,
    Conflict,
};

struct CoordinatePoolSlotLayout {
    CoordinatePoolSlotLayoutKind kind =
        CoordinatePoolSlotLayoutKind::Unknown;
    std::uint8_t logicalSlotCount = 0;
    std::uint8_t physicalSlotCount = 0;
    std::uint8_t phase = 0;

    constexpr bool IsLocked() const noexcept {
        return kind == CoordinatePoolSlotLayoutKind::Compact ||
            kind == CoordinatePoolSlotLayoutKind::Extended;
    }
};

inline constexpr CoordinatePoolSlotLayout kCoordinatePoolCompactLayout{
    CoordinatePoolSlotLayoutKind::Compact,
    static_cast<std::uint8_t>(kCoordinatePoolCompactLogicalSlotCount),
    static_cast<std::uint8_t>(
        kCoordinatePoolCompactLogicalSlotCount * kCoordinatePoolBankCount),
    static_cast<std::uint8_t>(kCoordinatePoolCompactLogicalSlotCount),
};

inline constexpr CoordinatePoolSlotLayout kCoordinatePoolExtendedLayout{
    CoordinatePoolSlotLayoutKind::Extended,
    static_cast<std::uint8_t>(kCoordinatePoolExtendedLogicalSlotCount),
    static_cast<std::uint8_t>(
        kCoordinatePoolExtendedLogicalSlotCount * kCoordinatePoolBankCount),
    static_cast<std::uint8_t>(kCoordinatePoolExtendedLogicalSlotCount),
};

constexpr bool IsCoordinatePoolSlotLayoutSupported(
    const CoordinatePoolSlotLayout& layout) noexcept {
    return layout.kind == CoordinatePoolSlotLayoutKind::Compact
        ? layout.logicalSlotCount ==
                kCoordinatePoolCompactLogicalSlotCount &&
            layout.physicalSlotCount ==
                kCoordinatePoolCompactLogicalSlotCount *
                    kCoordinatePoolBankCount &&
            layout.phase < layout.physicalSlotCount
        : layout.kind == CoordinatePoolSlotLayoutKind::Extended &&
            layout.logicalSlotCount ==
                kCoordinatePoolExtendedLogicalSlotCount &&
            layout.physicalSlotCount ==
                kCoordinatePoolExtendedLogicalSlotCount *
                    kCoordinatePoolBankCount &&
            layout.phase < layout.physicalSlotCount;
}

constexpr std::size_t MapDecodedCoordinatePoolSlot(
    std::size_t decodedSlot,
    const CoordinatePoolSlotLayout& layout) noexcept {
    if (!IsCoordinatePoolSlotLayoutSupported(layout) ||
        decodedSlot >= layout.physicalSlotCount) {
        return kCoordinatePoolPhysicalSlotCount;
    }
    return (decodedSlot + layout.phase) % layout.physicalSlotCount;
}

constexpr std::uint16_t CoordinatePoolTransitionMask(
    std::size_t previousDecodedSlot,
    std::size_t currentDecodedSlot,
    const CoordinatePoolSlotLayout& layout) noexcept {
    const std::size_t previous = MapDecodedCoordinatePoolSlot(
        previousDecodedSlot, layout);
    const std::size_t current = MapDecodedCoordinatePoolSlot(
        currentDecodedSlot, layout);
    if (previous >= kCoordinatePoolPhysicalSlotCount ||
        current >= kCoordinatePoolPhysicalSlotCount) {
        return 0;
    }
    return static_cast<std::uint16_t>(
        (UINT16_C(1) << previous) | (UINT16_C(1) << current));
}

constexpr bool ShouldRetryCoordinatePoolCompactSnapshot(
    const CoordinatePoolSlotLayout& layout,
    bool compactPossible,
    std::uint64_t decodedSlot,
    std::size_t attemptedPhysicalSlotCount,
    bool snapshotRead) noexcept {
    return !snapshotRead && !layout.IsLocked() && compactPossible &&
        decodedSlot < kCoordinatePoolCompactLayout.physicalSlotCount &&
        attemptedPhysicalSlotCount == kCoordinatePoolPhysicalSlotCount;
}

class CoordinatePoolSlotLayoutCalibration final {
public:
    CoordinatePoolSlotLayout ObserveDecodedSlot(
        std::uint64_t decodedSlot) noexcept {
        if (layout_.kind == CoordinatePoolSlotLayoutKind::Conflict) {
            return layout_;
        }
        if (decodedSlot >= kCoordinatePoolPhysicalSlotCount) {
            if (++invalidDecodedSlotCount_ >=
                kCoordinatePoolLayoutInvalidDecodedSlotLimit) {
                layout_.kind = CoordinatePoolSlotLayoutKind::Conflict;
            }
            return layout_;
        }
        invalidDecodedSlotCount_ = 0;
        decodedMask_ |= static_cast<std::uint16_t>(
            UINT16_C(1) << decodedSlot);
        if (layout_.IsLocked()) {
            if (decodedSlot >= layout_.physicalSlotCount) {
                if (layout_.kind == CoordinatePoolSlotLayoutKind::Compact) {
                    layout_ = {};
                    evidence_ = {};
                    evidenceCount_ = 0;
                    evidenceWriteIndex_ = 0;
                    compactPossible_ = false;
                } else {
                    layout_.kind = CoordinatePoolSlotLayoutKind::Conflict;
                }
            }
            return layout_;
        }
        if (decodedSlot >=
            kCoordinatePoolCompactLayout.physicalSlotCount) {
            compactPossible_ = false;
        }
        Resolve();
        return layout_;
    }

    CoordinatePoolSlotLayout ObserveTransition(
        std::uintptr_t component,
        std::uint64_t previousIndex,
        std::uint64_t currentIndex,
        std::uint64_t previousDecodedSlot,
        std::uint64_t currentDecodedSlot,
        std::uint16_t changedMask) noexcept {
        if (layout_.IsLocked() ||
            layout_.kind == CoordinatePoolSlotLayoutKind::Conflict ||
            component == 0 || previousIndex == currentIndex ||
            previousDecodedSlot == currentDecodedSlot) {
            return layout_;
        }
        if (ContainsEvidence(
                component,
                previousIndex,
                currentIndex,
                changedMask)) {
            return layout_;
        }
        const std::uint16_t compactChangedMask =
            changedMask & FullPhaseMask(
                kCoordinatePoolCompactLayout.physicalSlotCount);
        const std::uint16_t extendedChangedMask =
            changedMask & FullPhaseMask(
                kCoordinatePoolExtendedLayout.physicalSlotCount);
        const std::uint16_t compactMatches =
            BitCount(compactChangedMask) == 2
            ? MatchingPhases(
                  previousDecodedSlot,
                  currentDecodedSlot,
                  compactChangedMask,
                  kCoordinatePoolCompactLayout.physicalSlotCount)
            : 0;
        const std::uint16_t extendedMatches =
            BitCount(extendedChangedMask) == 2
            ? MatchingPhases(
                  previousDecodedSlot,
                  currentDecodedSlot,
                  extendedChangedMask,
                  kCoordinatePoolExtendedLayout.physicalSlotCount)
            : 0;
        if (compactMatches == 0 && extendedMatches == 0) return layout_;
        evidence_[evidenceWriteIndex_] = Evidence{
            component,
            previousIndex,
            currentIndex,
            changedMask,
            compactMatches,
            extendedMatches,
        };
        evidenceWriteIndex_ =
            (evidenceWriteIndex_ + 1) % evidence_.size();
        if (evidenceCount_ < evidence_.size()) ++evidenceCount_;
        Resolve();
        return layout_;
    }

    void Reset() noexcept { *this = {}; }

    CoordinatePoolSlotLayout Layout() const noexcept { return layout_; }
    std::uint16_t DecodedMask() const noexcept { return decodedMask_; }
    std::uint16_t CompactPhaseMask() const noexcept {
        return PhaseStatsFor(
            CoordinatePoolSlotLayoutKind::Compact).phaseMask;
    }
    std::uint16_t ExtendedPhaseMask() const noexcept {
        return PhaseStatsFor(
            CoordinatePoolSlotLayoutKind::Extended).phaseMask;
    }
    bool CompactPossible() const noexcept { return compactPossible_; }

    std::size_t EvidenceCount(
        CoordinatePoolSlotLayoutKind kind) const noexcept {
        if (kind == CoordinatePoolSlotLayoutKind::Compact &&
            !compactPossible_) {
            return 0;
        }
        std::size_t count = 0;
        for (std::size_t index = 0; index < evidenceCount_; ++index) {
            if ((kind == CoordinatePoolSlotLayoutKind::Compact &&
                 evidence_[index].compactPhases != 0) ||
                (kind == CoordinatePoolSlotLayoutKind::Extended &&
                 evidence_[index].extendedPhases != 0)) {
                ++count;
            }
        }
        return count;
    }

    std::size_t ComponentCount(
        CoordinatePoolSlotLayoutKind kind) const noexcept {
        return PhaseStatsFor(kind).componentCount;
    }

private:
    struct Evidence {
        std::uintptr_t component = 0;
        std::uint64_t previousIndex = 0;
        std::uint64_t currentIndex = 0;
        std::uint16_t changedMask = 0;
        std::uint16_t compactPhases = 0;
        std::uint16_t extendedPhases = 0;
    };

    struct PhaseStats {
        std::uint16_t phaseMask = 0;
        std::size_t bestEvidence = 0;
        std::size_t runnerUpEvidence = 0;
        std::size_t componentCount = 0;
        std::uint8_t phase = 0;
        bool unique = false;
        bool possible = false;
    };

    static constexpr std::size_t BitCount(std::uint16_t value) noexcept {
        std::size_t count = 0;
        while (value != 0) {
            value &= static_cast<std::uint16_t>(value - 1U);
            ++count;
        }
        return count;
    }

    static constexpr std::uint16_t FullPhaseMask(
        std::size_t physicalSlotCount) noexcept {
        return static_cast<std::uint16_t>(
            (UINT32_C(1) << physicalSlotCount) - 1U);
    }

    static constexpr std::uint16_t MatchingPhases(
        std::uint64_t previousDecodedSlot,
        std::uint64_t currentDecodedSlot,
        std::uint16_t changedMask,
        std::size_t physicalSlotCount) noexcept {
        if (physicalSlotCount == 0 ||
            previousDecodedSlot >= physicalSlotCount ||
            currentDecodedSlot >= physicalSlotCount) {
            return 0;
        }
        std::uint16_t phases = 0;
        for (std::size_t phase = 0;
             phase < physicalSlotCount;
             ++phase) {
            const std::size_t previous =
                (previousDecodedSlot + phase) % physicalSlotCount;
            const std::size_t current =
                (currentDecodedSlot + phase) % physicalSlotCount;
            const std::uint16_t expected = static_cast<std::uint16_t>(
                (UINT16_C(1) << previous) |
                (UINT16_C(1) << current));
            if (expected == changedMask) {
                phases |= static_cast<std::uint16_t>(UINT16_C(1) << phase);
            }
        }
        return phases;
    }

    bool ContainsEvidence(
        std::uintptr_t component,
        std::uint64_t previousIndex,
        std::uint64_t currentIndex,
        std::uint16_t changedMask) const noexcept {
        for (std::size_t index = 0; index < evidenceCount_; ++index) {
            const Evidence& item = evidence_[index];
            if (item.component == component &&
                item.previousIndex == previousIndex &&
                item.currentIndex == currentIndex &&
                item.changedMask == changedMask) {
                return true;
            }
        }
        return false;
    }

    std::size_t ComponentCountForPhase(
        CoordinatePoolSlotLayoutKind kind,
        std::uint8_t phase) const noexcept {
        std::array<std::uintptr_t, kCoordinatePoolLayoutEvidenceLimit>
            components{};
        std::size_t count = 0;
        const std::uint16_t phaseBit = static_cast<std::uint16_t>(
            UINT16_C(1) << phase);
        for (std::size_t index = 0; index < evidenceCount_; ++index) {
            const std::uint16_t phases =
                kind == CoordinatePoolSlotLayoutKind::Compact
                ? evidence_[index].compactPhases
                : evidence_[index].extendedPhases;
            if ((phases & phaseBit) == 0) continue;
            bool found = false;
            for (std::size_t componentIndex = 0;
                 componentIndex < count;
                 ++componentIndex) {
                if (components[componentIndex] ==
                    evidence_[index].component) {
                    found = true;
                    break;
                }
            }
            if (!found) components[count++] = evidence_[index].component;
        }
        return count;
    }

    PhaseStats PhaseStatsFor(
        CoordinatePoolSlotLayoutKind kind) const noexcept {
        PhaseStats stats{};
        const bool compact =
            kind == CoordinatePoolSlotLayoutKind::Compact;
        if ((!compact &&
             kind != CoordinatePoolSlotLayoutKind::Extended) ||
            (compact && !compactPossible_)) {
            return stats;
        }
        stats.possible = true;
        const std::size_t physicalSlotCount = compact
            ? kCoordinatePoolCompactLayout.physicalSlotCount
            : kCoordinatePoolExtendedLayout.physicalSlotCount;
        std::array<std::size_t, kCoordinatePoolPhysicalSlotCount> votes{};
        for (std::size_t index = 0; index < evidenceCount_; ++index) {
            const std::uint16_t phases = compact
                ? evidence_[index].compactPhases
                : evidence_[index].extendedPhases;
            for (std::size_t phase = 0;
                 phase < physicalSlotCount;
                 ++phase) {
                if ((phases & (UINT16_C(1) << phase)) != 0) {
                    ++votes[phase];
                }
            }
        }
        for (std::size_t phase = 0;
             phase < physicalSlotCount;
             ++phase) {
            if (votes[phase] > stats.bestEvidence) {
                stats.runnerUpEvidence = stats.bestEvidence;
                stats.bestEvidence = votes[phase];
                stats.phaseMask = static_cast<std::uint16_t>(
                    UINT16_C(1) << phase);
            } else if (votes[phase] == stats.bestEvidence) {
                stats.phaseMask |= static_cast<std::uint16_t>(
                    UINT16_C(1) << phase);
            } else if (votes[phase] > stats.runnerUpEvidence) {
                stats.runnerUpEvidence = votes[phase];
            }
        }
        if (stats.bestEvidence == 0) {
            stats.phaseMask = FullPhaseMask(physicalSlotCount);
            return stats;
        }
        stats.unique = BitCount(stats.phaseMask) == 1;
        if (!stats.unique) {
            stats.runnerUpEvidence = stats.bestEvidence;
            return stats;
        }
        for (std::size_t phase = 0;
             phase < physicalSlotCount;
             ++phase) {
            if ((stats.phaseMask & (UINT16_C(1) << phase)) != 0) {
                stats.phase = static_cast<std::uint8_t>(phase);
                break;
            }
        }
        stats.componentCount = ComponentCountForPhase(kind, stats.phase);
        return stats;
    }

    void Resolve() noexcept {
        if (layout_.IsLocked() ||
            layout_.kind == CoordinatePoolSlotLayoutKind::Conflict) {
            return;
        }
        const PhaseStats compact = PhaseStatsFor(
            CoordinatePoolSlotLayoutKind::Compact);
        const PhaseStats extended = PhaseStatsFor(
            CoordinatePoolSlotLayoutKind::Extended);
        const PhaseStats* winner = nullptr;
        const PhaseStats* other = nullptr;
        CoordinatePoolSlotLayout selected{};
        if (compact.unique &&
            (!extended.unique ||
             compact.bestEvidence > extended.bestEvidence)) {
            winner = &compact;
            other = &extended;
            selected = kCoordinatePoolCompactLayout;
        } else if (extended.unique &&
            (!compact.unique ||
             extended.bestEvidence > compact.bestEvidence)) {
            winner = &extended;
            other = &compact;
            selected = kCoordinatePoolExtendedLayout;
        } else {
            return;
        }
        const bool enoughComponents =
            winner->componentCount >=
                kCoordinatePoolLayoutMinimumComponents ||
            (winner->componentCount >= 1 &&
             winner->bestEvidence >=
                kCoordinatePoolLayoutSingleComponentEvidence);
        const std::size_t competingEvidence =
            winner->runnerUpEvidence > other->bestEvidence
            ? winner->runnerUpEvidence
            : other->bestEvidence;
        if (winner->bestEvidence <
                kCoordinatePoolLayoutMinimumEvidence ||
            !enoughComponents ||
            winner->bestEvidence <
                competingEvidence + kCoordinatePoolLayoutMinimumLead) {
            return;
        }
        selected.phase = winner->phase;
        layout_ = selected;
    }

    CoordinatePoolSlotLayout layout_{};
    std::array<Evidence, kCoordinatePoolLayoutEvidenceLimit> evidence_{};
    std::size_t evidenceCount_ = 0;
    std::size_t evidenceWriteIndex_ = 0;
    std::uint16_t decodedMask_ = 0;
    std::size_t invalidDecodedSlotCount_ = 0;
    bool compactPossible_ = true;
};

constexpr bool CoordinatePoolEnvironmentFlagEnabled(
    const char* value) noexcept {
    return value != nullptr && value[0] == '1';
}

constexpr bool IsCoordinatePoolReadRangeValid(
    std::uint64_t address,
    std::size_t size) noexcept {
    return size != 0 && address >= kCoordinatePoolMinimumRemoteAddress &&
        address < kCoordinatePoolMaximumRemoteAddress &&
        size <= kCoordinatePoolMaximumRemoteAddress - address;
}

struct CoordinatePoolRootSnapshot {
    std::uint64_t bridge = 0;
    std::uint64_t context = 0;
    std::uint64_t entry = 0;
};

constexpr bool CoordinatePoolRootSnapshotsMatch(
    const CoordinatePoolRootSnapshot& left,
    const CoordinatePoolRootSnapshot& right) noexcept {
    return left.bridge == right.bridge && left.context == right.context &&
        left.entry == right.entry;
}

constexpr bool IsCoordinatePoolRootSnapshotInitialized(
    const CoordinatePoolRootSnapshot& snapshot) noexcept {
    return snapshot.bridge != 0 && snapshot.context != 0 &&
        snapshot.entry != 0;
}

constexpr bool CoordinatePoolGuardedRootSnapshotMatches(
    const CoordinatePoolRootSnapshot& accepted,
    const CoordinatePoolRootSnapshot& observed,
    std::uint64_t trailingBridge) noexcept {
    return IsCoordinatePoolRootSnapshotInitialized(accepted) &&
        CoordinatePoolRootSnapshotsMatch(accepted, observed) &&
        observed.bridge == trailingBridge;
}

constexpr bool CoordinatePoolCodeIdentityChanged(
    const CoordinatePoolRootSnapshot& previous,
    const CoordinatePoolRootSnapshot& current) noexcept {
    return previous.entry != current.entry;
}

constexpr bool CoordinatePoolContextIdentityChanged(
    const CoordinatePoolRootSnapshot& previous,
    const CoordinatePoolRootSnapshot& current) noexcept {
    return previous.bridge != current.bridge ||
        previous.context != current.context;
}

class CoordinatePoolRootStabilityWindow final {
public:
    bool Observe(const CoordinatePoolRootSnapshot& snapshot) noexcept {
        const bool stable = ready_ &&
            CoordinatePoolRootSnapshotsMatch(previous_, snapshot);
        previous_ = snapshot;
        ready_ = true;
        return stable;
    }

    void Reset() noexcept {
        previous_ = {};
        ready_ = false;
    }

private:
    CoordinatePoolRootSnapshot previous_{};
    bool ready_ = false;
};

constexpr std::uint64_t NormalizeCoordinatePoolPointer(
    std::uint64_t value) noexcept {
    return value & kCoordinatePoolPointerPayloadMask;
}

constexpr std::uint64_t NormalizeCoordinatePoolIndexedPointer(
    std::uint64_t value) noexcept {
    return value & kCoordinatePoolIndexedPointerPayloadMask;
}

constexpr bool ResolveCoordinatePoolIndexedPointerAddress(
    std::uint64_t pointer,
    std::int64_t offset,
    std::uint64_t& address) noexcept {
    const std::uint64_t base =
        NormalizeCoordinatePoolIndexedPointer(pointer);
    if (offset >= 0) {
        const std::uint64_t addition =
            static_cast<std::uint64_t>(offset);
        if (addition > UINT64_MAX - base) return false;
        address = base + addition;
        return true;
    }
    const std::uint64_t subtraction =
        static_cast<std::uint64_t>(-(offset + 1)) + 1;
    if (subtraction > base) return false;
    address = base - subtraction;
    return true;
}

constexpr bool ResolveCoordinatePoolIndexedRootAddresses(
    std::uint64_t bridge,
    std::int64_t contextOffset,
    std::uint64_t entryOffset,
    std::uint64_t& contextAddress,
    std::uint64_t& entryAddress) noexcept {
    return ResolveCoordinatePoolIndexedPointerAddress(
               bridge, contextOffset, contextAddress) &&
        ResolveCoordinatePoolIndexedPointerAddress(
               bridge,
               static_cast<std::int64_t>(entryOffset),
               entryAddress);
}

inline std::uint64_t CoordinatePoolCodeFingerprint(
    const std::uint8_t* bytes,
    std::size_t size) noexcept {
    constexpr std::uint64_t kOffsetBasis = UINT64_C(14695981039346656037);
    constexpr std::uint64_t kPrime = UINT64_C(1099511628211);
    std::uint64_t fingerprint = kOffsetBasis;
    if (bytes == nullptr) return size == 0 ? fingerprint : 0;
    for (std::size_t index = 0; index < size; ++index) {
        fingerprint ^= bytes[index];
        fingerprint *= kPrime;
    }
    return fingerprint;
}

constexpr bool ShouldValidateCoordinatePoolCode(
    std::uint64_t frame,
    std::uint64_t nextValidationFrame,
    bool requested) noexcept {
    if (requested) return true;
    if (nextValidationFrame == kCoordinatePoolCodeValidationIdleFrame) {
        return false;
    }
    return frame >= nextValidationFrame ||
        nextValidationFrame - frame >
            kCoordinatePoolCodeValidationRetryFrames;
}

constexpr std::uint64_t NextCoordinatePoolCodeValidationFrame(
    std::uint64_t frame,
    bool validationSucceeded) noexcept {
    if (validationSucceeded) {
        return kCoordinatePoolCodeValidationIdleFrame;
    }
    return frame > UINT64_MAX - kCoordinatePoolCodeValidationRetryFrames
        ? UINT64_MAX
        : frame + kCoordinatePoolCodeValidationRetryFrames;
}

constexpr bool ShouldClearCoordinatePoolRingsAfterPointerRefresh(
    bool refreshSucceeded,
    std::uint64_t previousPointer,
    std::uint64_t refreshedPointer) noexcept {
    return refreshSucceeded && previousPointer != 0 &&
        previousPointer != refreshedPointer;
}

constexpr bool ShouldRetryCoordinatePoolRing(
    std::uint64_t stamp,
    std::uint64_t frame) noexcept {
    return frame < stamp || frame - stamp >= kCoordinatePoolRingRetryFrames;
}

constexpr bool ShouldSearchCoordinatePoolRing(
    bool hasSlot,
    bool hasRing,
    std::uint64_t stamp,
    std::uint64_t frame) noexcept {
    return !hasRing &&
        (!hasSlot || ShouldRetryCoordinatePoolRing(stamp, frame));
}

enum class CoordinatePoolRingReadEvent : std::uint8_t {
    Success,
    RemoteReadFailure,
    OtherFailure,
};

class CoordinatePoolRingRecoveryState final {
public:
    bool Observe(CoordinatePoolRingReadEvent event) noexcept {
        if (event == CoordinatePoolRingReadEvent::Success) {
            failures_ = 0;
            return false;
        }
        if (event != CoordinatePoolRingReadEvent::RemoteReadFailure) {
            return false;
        }
        if (failures_ < kCoordinatePoolRingReadFailureThreshold) {
            ++failures_;
        }
        return failures_ >= kCoordinatePoolRingReadFailureThreshold;
    }

    void Reset() noexcept {
        failures_ = 0;
    }

    std::uint8_t Failures() const noexcept {
        return failures_;
    }

private:
    std::uint8_t failures_ = 0;
};

class CoordinatePoolRingSearchBudget final {
public:
    bool TryConsume(std::uint64_t frame) noexcept {
        if (!ready_ || frame_ != frame) {
            frame_ = frame;
            used_ = 0;
            ready_ = true;
        }
        if (used_ >= kCoordinatePoolRingSearchesPerFrame) return false;
        ++used_;
        return true;
    }

    void Reset() noexcept {
        frame_ = 0;
        used_ = 0;
        ready_ = false;
    }

private:
    std::uint64_t frame_ = 0;
    std::size_t used_ = 0;
    bool ready_ = false;
};

}  // namespace lengjing::game::native
