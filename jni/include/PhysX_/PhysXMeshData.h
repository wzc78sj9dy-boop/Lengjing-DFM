//
// Created by binglen on 2025/6/8.
//

#ifndef PHYSX_MESH_DATA_H
#define PHYSX_MESH_DATA_H

#include "PhysXStructures.h"
#include "PhysX_Log.h"
#include <unordered_set>
#include <fstream>
#include <sys/stat.h>
#include <algorithm>

struct TriangleMeshData {
	std::vector<physx::PxVec3> Vertices{};
	std::vector<uint32_t> Indices{};
	uint8_t Flags{};
	FilterDataT QueryFilterData{};
	FilterDataT SimulationFilterData{};
	PrunerPayload UniqueKey1;
	uint64_t UniqueKey2;
	PrunerPayload UniqueKey3;
	PxGeometryType Type{};
	physx::PxTransform Transform;
	size_t HashValue = 0;

	size_t ComputeHash() const {
		size_t hash = 0;

		for (const auto& v : Vertices) {
			hash_combine(hash, v.x);
			hash_combine(hash, v.y);
			hash_combine(hash, v.z);
		}

		for (const auto& idx : Indices) {
			hash_combine(hash, idx);
		}

		hash_combine(hash, Type);
		hash_combine(hash, Flags);
		hash_combine(hash, QueryFilterData.word0);
		hash_combine(hash, QueryFilterData.word1);
		hash_combine(hash, QueryFilterData.word2);
		hash_combine(hash, QueryFilterData.word3);

		return hash;
	}

    bool IsEqual(const TriangleMeshData& other) const {
        if (Vertices.size() != other.Vertices.size() ||
            Indices.size() != other.Indices.size() ||
            Type != other.Type ||
            Flags != other.Flags ||
            memcmp(&QueryFilterData, &other.QueryFilterData, sizeof(FilterDataT)) != 0 ||
            memcmp(&SimulationFilterData, &other.SimulationFilterData, sizeof(FilterDataT)) != 0) {
            return false;
        }

        if (memcmp(Vertices.data(), other.Vertices.data(), Vertices.size() * sizeof(physx::PxVec3)) != 0) {
            return false;
        }

        if (memcmp(Indices.data(), other.Indices.data(), Indices.size() * sizeof(uint32_t)) != 0) {
            return false;
        }

        return true;
    }
};

#endif