#pragma once

#include <cstddef>
#include <cstdint>

namespace lengjing::game::native {

struct BoneFrameRecordSource {
    std::uintptr_t root = 0;
    std::uintptr_t mesh = 0;
    bool encryptedRecord = false;
    bool resolverRecord = false;
};

struct BoneFrameCacheSource {
    std::uintptr_t root = 0;
    std::uintptr_t mesh = 0;
    bool encryptedRecord = false;
};

struct BoneFrameSourceSelection {
    std::uintptr_t root = 0;
    std::uintptr_t mesh = 0;
    bool rebuildResolvedTransform = false;

    constexpr explicit operator bool() const noexcept {
        return mesh != 0;
    }
};

constexpr bool IsResolvedBoneTransformEnabled(
    bool firstDecryptEnabled,
    bool secondDecryptEnabled) noexcept {
    return firstDecryptEnabled || secondDecryptEnabled;
}

constexpr bool PreferBoneFrameCandidate(
    std::size_t currentValidCount,
    bool currentUsable,
    std::size_t candidateValidCount,
    bool candidateUsable) noexcept {
    if (candidateUsable != currentUsable) return candidateUsable;
    return candidateValidCount > currentValidCount;
}

constexpr bool ShouldReadSecondaryBoneArray(
    bool rebuildResolvedTransform,
    bool primaryBoneArrayAvailable,
    std::size_t primaryValidCount,
    std::size_t expectedCount) noexcept {
    return rebuildResolvedTransform
        ? !primaryBoneArrayAvailable
        : primaryValidCount < expectedCount;
}

inline BoneFrameSourceSelection SelectPreferredBoneFrameSource(
    const BoneFrameRecordSource& record,
    std::uintptr_t ordinaryRoot,
    std::uintptr_t ordinaryMesh,
    bool resolvedBoneTransformEnabled) noexcept {
    if (ordinaryMesh != 0) {
        return BoneFrameSourceSelection{
            ordinaryRoot,
            ordinaryMesh,
            false,
        };
    }
    if (record.encryptedRecord && !resolvedBoneTransformEnabled) {
        return {};
    }
    return BoneFrameSourceSelection{
        record.root,
        record.mesh,
        record.encryptedRecord,
    };
}

inline BoneFrameSourceSelection SelectFallbackBoneFrameSource(
    const BoneFrameRecordSource& record,
    std::uintptr_t ordinaryRoot,
    std::uintptr_t ordinaryMesh,
    bool resolvedBoneTransformEnabled) noexcept {
    if (ordinaryMesh == 0 || record.mesh == 0 ||
        !record.resolverRecord ||
        (record.encryptedRecord && !resolvedBoneTransformEnabled)) {
        return {};
    }
    const BoneFrameSourceSelection preferred =
        SelectPreferredBoneFrameSource(
            record,
            ordinaryRoot,
            ordinaryMesh,
            resolvedBoneTransformEnabled);
    if (preferred.root == record.root && preferred.mesh == record.mesh &&
        preferred.rebuildResolvedTransform == record.encryptedRecord) {
        return {};
    }
    return BoneFrameSourceSelection{
        record.root,
        record.mesh,
        record.encryptedRecord,
    };
}

inline std::uintptr_t SelectBoneFrameMesh(
    const BoneFrameRecordSource& record,
    std::uintptr_t ordinaryMesh,
    bool resolvedBoneTransformEnabled) noexcept {
    return SelectPreferredBoneFrameSource(
               record,
               0,
               ordinaryMesh,
               resolvedBoneTransformEnabled)
        .mesh;
}

inline bool MatchesBoneFrameCacheSource(
    const BoneFrameSourceSelection& source,
    const BoneFrameCacheSource& cache) noexcept {
    if (!source || cache.mesh != source.mesh ||
        cache.encryptedRecord != source.rebuildResolvedTransform) {
        return false;
    }
    return !source.rebuildResolvedTransform ||
        (source.root != 0 && cache.root == source.root);
}

inline bool ShouldResetBoneFrameCache(
    const BoneFrameSourceSelection& source,
    std::uintptr_t boneArray,
    bool resolvedTranslation,
    const BoneFrameCacheSource& cache,
    std::uintptr_t cachedBoneArray,
    bool cachedResolvedTranslation) noexcept {
    return !MatchesBoneFrameCacheSource(source, cache) ||
        (boneArray != 0 && cachedBoneArray != 0 &&
         boneArray != cachedBoneArray) ||
        (cache.mesh != 0 &&
         resolvedTranslation != cachedResolvedTranslation);
}

inline bool IsBoneFrameCacheSourceCompatible(
    const BoneFrameRecordSource& record,
    std::uintptr_t ordinaryRoot,
    std::uintptr_t ordinaryMesh,
    bool resolvedBoneTransformEnabled,
    const BoneFrameCacheSource& cache) noexcept {
    return MatchesBoneFrameCacheSource(
               SelectPreferredBoneFrameSource(
                   record,
                   ordinaryRoot,
                   ordinaryMesh,
                   resolvedBoneTransformEnabled),
               cache) ||
        MatchesBoneFrameCacheSource(
            SelectFallbackBoneFrameSource(
                record,
                ordinaryRoot,
                ordinaryMesh,
                resolvedBoneTransformEnabled),
            cache);
}

}  // namespace lengjing::game::native
