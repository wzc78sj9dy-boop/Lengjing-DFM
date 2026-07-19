#pragma once

#include <cstdint>

namespace lengjing::game::native {

struct BoneFrameRecordSource {
    std::uintptr_t root = 0;
    std::uintptr_t mesh = 0;
    bool encryptedRecord = false;
};

struct BoneFrameCacheSource {
    std::uintptr_t root = 0;
    std::uintptr_t mesh = 0;
    bool encryptedRecord = false;
};

inline std::uintptr_t SelectBoneFrameMesh(
    const BoneFrameRecordSource& record,
    std::uintptr_t ordinaryMesh) noexcept {
    if (record.encryptedRecord) return record.mesh;
    return ordinaryMesh != 0 ? ordinaryMesh : record.mesh;
}

inline bool IsBoneFrameCacheSourceCompatible(
    const BoneFrameRecordSource& record,
    std::uintptr_t liveMesh,
    const BoneFrameCacheSource& cache) noexcept {
    if (cache.mesh == 0 ||
        cache.encryptedRecord != record.encryptedRecord) {
        return false;
    }
    if (record.encryptedRecord) {
        return record.root != 0 && record.mesh != 0 &&
            cache.root == record.root && cache.mesh == record.mesh;
    }

    const std::uintptr_t expectedMesh = liveMesh != 0
        ? liveMesh
        : record.mesh;
    return expectedMesh == 0 || cache.mesh == expectedMesh;
}

}  // namespace lengjing::game::native
