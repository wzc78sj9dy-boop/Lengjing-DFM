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
inline constexpr std::uint64_t kCoordinatePoolMinimumRemoteAddress =
    UINT64_C(0x10000000);
inline constexpr std::uint64_t kCoordinatePoolMaximumRemoteAddress =
    UINT64_C(0x10000000000);

inline constexpr std::size_t kCoordinatePoolCompactLogicalSlotCount = 5;
inline constexpr std::size_t kCoordinatePoolIntermediateLogicalSlotCount = 6;
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

enum class CoordinatePoolSlotLayoutKind : std::uint8_t {
    Unknown,
    Compact,
    Extended,
    Conflict,
    Intermediate,
};

struct CoordinatePoolSlotLayout {
    CoordinatePoolSlotLayoutKind kind =
        CoordinatePoolSlotLayoutKind::Unknown;
    std::uint8_t logicalSlotCount = 0;
    std::uint8_t physicalSlotCount = 0;
    std::uint8_t phase = 0;

    constexpr bool IsLocked() const noexcept {
        return kind == CoordinatePoolSlotLayoutKind::Compact ||
            kind == CoordinatePoolSlotLayoutKind::Intermediate ||
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

inline constexpr CoordinatePoolSlotLayout kCoordinatePoolIntermediateLayout{
    CoordinatePoolSlotLayoutKind::Intermediate,
    static_cast<std::uint8_t>(kCoordinatePoolIntermediateLogicalSlotCount),
    static_cast<std::uint8_t>(
        kCoordinatePoolIntermediateLogicalSlotCount *
        kCoordinatePoolBankCount),
    static_cast<std::uint8_t>(kCoordinatePoolIntermediateLogicalSlotCount),
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
    switch (layout.kind) {
        case CoordinatePoolSlotLayoutKind::Compact:
            return layout.logicalSlotCount ==
                    kCoordinatePoolCompactLogicalSlotCount &&
                layout.physicalSlotCount ==
                    kCoordinatePoolCompactLogicalSlotCount *
                        kCoordinatePoolBankCount &&
                layout.phase < layout.physicalSlotCount;
        case CoordinatePoolSlotLayoutKind::Intermediate:
            return layout.logicalSlotCount ==
                    kCoordinatePoolIntermediateLogicalSlotCount &&
                layout.physicalSlotCount ==
                    kCoordinatePoolIntermediateLogicalSlotCount *
                        kCoordinatePoolBankCount &&
                layout.phase < layout.physicalSlotCount;
        case CoordinatePoolSlotLayoutKind::Extended:
            return layout.logicalSlotCount ==
                    kCoordinatePoolExtendedLogicalSlotCount &&
                layout.physicalSlotCount ==
                    kCoordinatePoolExtendedLogicalSlotCount *
                        kCoordinatePoolBankCount &&
                layout.phase < layout.physicalSlotCount;
        default:
            return false;
    }
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

constexpr std::size_t CoordinatePoolSnapshotRetrySlotCount(
    const CoordinatePoolSlotLayout& layout,
    bool compactPossible,
    bool intermediatePossible,
    std::uint64_t decodedSlot,
    std::size_t attemptedPhysicalSlotCount,
    bool snapshotRead) noexcept {
    if (snapshotRead || layout.IsLocked()) {
        return attemptedPhysicalSlotCount;
    }
    if (attemptedPhysicalSlotCount == kCoordinatePoolPhysicalSlotCount &&
        intermediatePossible &&
        decodedSlot < kCoordinatePoolIntermediateLayout.physicalSlotCount) {
        return kCoordinatePoolIntermediateLayout.physicalSlotCount;
    }
    if (attemptedPhysicalSlotCount >
            kCoordinatePoolCompactLayout.physicalSlotCount &&
        compactPossible &&
        decodedSlot < kCoordinatePoolCompactLayout.physicalSlotCount) {
        return kCoordinatePoolCompactLayout.physicalSlotCount;
    }
    return attemptedPhysicalSlotCount;
}

constexpr bool ShouldRestartCoordinatePoolSnapshotAfterLayoutChange(
    bool capturedPhysicalSlots,
    const CoordinatePoolSlotLayout& previousLayout,
    const CoordinatePoolSlotLayout& currentLayout) noexcept {
    return !capturedPhysicalSlots && previousLayout.IsLocked() &&
        currentLayout.kind == CoordinatePoolSlotLayoutKind::Unknown;
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
                if (layout_.kind != CoordinatePoolSlotLayoutKind::Extended) {
                    layout_ = {};
                    evidence_ = {};
                    evidenceCount_ = 0;
                    evidenceWriteIndex_ = 0;
                } else {
                    layout_.kind = CoordinatePoolSlotLayoutKind::Conflict;
                }
            }
        }
        if (decodedSlot >=
            kCoordinatePoolCompactLayout.physicalSlotCount) {
            compactPossible_ = false;
        }
        if (decodedSlot >=
            kCoordinatePoolIntermediateLayout.physicalSlotCount) {
            intermediatePossible_ = false;
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
        const std::uint16_t intermediateChangedMask =
            changedMask & FullPhaseMask(
                kCoordinatePoolIntermediateLayout.physicalSlotCount);
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
        const std::uint16_t intermediateMatches =
            BitCount(intermediateChangedMask) == 2
            ? MatchingPhases(
                  previousDecodedSlot,
                  currentDecodedSlot,
                  intermediateChangedMask,
                  kCoordinatePoolIntermediateLayout.physicalSlotCount)
            : 0;
        const std::uint16_t extendedMatches =
            BitCount(extendedChangedMask) == 2
            ? MatchingPhases(
                  previousDecodedSlot,
                  currentDecodedSlot,
                  extendedChangedMask,
                  kCoordinatePoolExtendedLayout.physicalSlotCount)
            : 0;
        if (compactMatches == 0 && intermediateMatches == 0 &&
            extendedMatches == 0) {
            return layout_;
        }
        evidence_[evidenceWriteIndex_] = Evidence{
            component,
            previousIndex,
            currentIndex,
            changedMask,
            compactMatches,
            intermediateMatches,
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
    std::uint16_t IntermediatePhaseMask() const noexcept {
        return PhaseStatsFor(
            CoordinatePoolSlotLayoutKind::Intermediate).phaseMask;
    }
    std::uint16_t ExtendedPhaseMask() const noexcept {
        return PhaseStatsFor(
            CoordinatePoolSlotLayoutKind::Extended).phaseMask;
    }
    bool CompactPossible() const noexcept { return compactPossible_; }
    bool IntermediatePossible() const noexcept {
        return intermediatePossible_;
    }

    std::size_t EvidenceCount(
        CoordinatePoolSlotLayoutKind kind) const noexcept {
        if (!LayoutPossible(kind)) {
            return 0;
        }
        std::size_t count = 0;
        for (std::size_t index = 0; index < evidenceCount_; ++index) {
            if (EvidencePhases(evidence_[index], kind) != 0) {
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
        std::uint16_t intermediatePhases = 0;
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

    static constexpr CoordinatePoolSlotLayout BaseLayout(
        CoordinatePoolSlotLayoutKind kind) noexcept {
        switch (kind) {
            case CoordinatePoolSlotLayoutKind::Compact:
                return kCoordinatePoolCompactLayout;
            case CoordinatePoolSlotLayoutKind::Intermediate:
                return kCoordinatePoolIntermediateLayout;
            case CoordinatePoolSlotLayoutKind::Extended:
                return kCoordinatePoolExtendedLayout;
            default:
                return {};
        }
    }

    bool LayoutPossible(
        CoordinatePoolSlotLayoutKind kind) const noexcept {
        switch (kind) {
            case CoordinatePoolSlotLayoutKind::Compact:
                return compactPossible_;
            case CoordinatePoolSlotLayoutKind::Intermediate:
                return intermediatePossible_;
            case CoordinatePoolSlotLayoutKind::Extended:
                return true;
            default:
                return false;
        }
    }

    static constexpr std::uint16_t EvidencePhases(
        const Evidence& evidence,
        CoordinatePoolSlotLayoutKind kind) noexcept {
        switch (kind) {
            case CoordinatePoolSlotLayoutKind::Compact:
                return evidence.compactPhases;
            case CoordinatePoolSlotLayoutKind::Intermediate:
                return evidence.intermediatePhases;
            case CoordinatePoolSlotLayoutKind::Extended:
                return evidence.extendedPhases;
            default:
                return 0;
        }
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
            const std::uint16_t phases = EvidencePhases(
                evidence_[index], kind);
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
        if (!LayoutPossible(kind)) {
            return stats;
        }
        const CoordinatePoolSlotLayout baseLayout = BaseLayout(kind);
        if (!baseLayout.IsLocked()) return stats;
        stats.possible = true;
        const std::size_t physicalSlotCount =
            baseLayout.physicalSlotCount;
        std::array<std::size_t, kCoordinatePoolPhysicalSlotCount> votes{};
        for (std::size_t index = 0; index < evidenceCount_; ++index) {
            const std::uint16_t phases = EvidencePhases(
                evidence_[index], kind);
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
        const PhaseStats intermediate = PhaseStatsFor(
            CoordinatePoolSlotLayoutKind::Intermediate);
        const PhaseStats extended = PhaseStatsFor(
            CoordinatePoolSlotLayoutKind::Extended);
        const std::array<CoordinatePoolSlotLayoutKind, 3> kinds{{
            CoordinatePoolSlotLayoutKind::Compact,
            CoordinatePoolSlotLayoutKind::Intermediate,
            CoordinatePoolSlotLayoutKind::Extended,
        }};
        const std::array<const PhaseStats*, 3> candidates{{
            &compact,
            &intermediate,
            &extended,
        }};
        std::size_t winnerIndex = candidates.size();
        bool winnerTied = false;
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            if (!candidates[index]->unique) continue;
            if (winnerIndex == candidates.size() ||
                candidates[index]->bestEvidence >
                    candidates[winnerIndex]->bestEvidence) {
                winnerIndex = index;
                winnerTied = false;
            } else if (candidates[index]->bestEvidence ==
                       candidates[winnerIndex]->bestEvidence) {
                winnerTied = true;
            }
        }
        if (winnerIndex == candidates.size() || winnerTied) return;
        const PhaseStats* winner = candidates[winnerIndex];
        const bool enoughComponents =
            winner->componentCount >=
                kCoordinatePoolLayoutMinimumComponents ||
            (winner->componentCount >= 1 &&
             winner->bestEvidence >=
                kCoordinatePoolLayoutSingleComponentEvidence);
        std::size_t competingEvidence = winner->runnerUpEvidence;
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            if (index != winnerIndex &&
                candidates[index]->bestEvidence > competingEvidence) {
                competingEvidence = candidates[index]->bestEvidence;
            }
        }
        if (winner->bestEvidence <
                kCoordinatePoolLayoutMinimumEvidence ||
            !enoughComponents ||
            winner->bestEvidence <
                competingEvidence + kCoordinatePoolLayoutMinimumLead) {
            return;
        }
        CoordinatePoolSlotLayout selected = BaseLayout(
            kinds[winnerIndex]);
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
    bool intermediatePossible_ = true;
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
