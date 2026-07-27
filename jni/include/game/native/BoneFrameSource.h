#pragma once

#include <cstddef>
#include <cstdint>

namespace lengjing::game::native {

struct BoneFrameCacheSource {
    std::uintptr_t root = 0;
    std::uintptr_t mesh = 0;
};

struct BoneFrameSourceSelection {
    std::uintptr_t root = 0;
    std::uintptr_t mesh = 0;

    constexpr explicit operator bool() const noexcept {
        return mesh != 0;
    }
};

constexpr bool PreferBoneFrameCandidate(
    std::size_t currentValidCount,
    bool currentUsable,
    std::size_t candidateValidCount,
    bool candidateUsable) noexcept {
    if (candidateUsable != currentUsable) return candidateUsable;
    return candidateValidCount > currentValidCount;
}

constexpr bool ShouldReadSecondaryBoneArray(
    std::size_t primaryValidCount,
    std::size_t expectedCount) noexcept {
    return primaryValidCount < expectedCount;
}

inline BoneFrameSourceSelection SelectBoneFrameSource(
    std::uintptr_t root,
    std::uintptr_t mesh) noexcept {
    return BoneFrameSourceSelection{root, mesh};
}

inline bool MatchesBoneFrameCacheSource(
    const BoneFrameSourceSelection& source,
    const BoneFrameCacheSource& cache) noexcept {
    return source && cache.root == source.root && cache.mesh == source.mesh;
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
    const BoneFrameSourceSelection& source,
    const BoneFrameCacheSource& cache) noexcept {
    return MatchesBoneFrameCacheSource(source, cache);
}

}  // namespace lengjing::game::native
