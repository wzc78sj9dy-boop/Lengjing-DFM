//
// Created by binglen on 2025/6/8.
//

#ifndef PHYSX_COLLECTOR_H
#define PHYSX_COLLECTOR_H

#include "PhysXStructures.h"
#include "PhysXEnums.h"
#include "PhysXLoader.h"
#include "PhysX_Log.h"
#include <vector>
#include <set>
#include <unordered_map>
#include <cstring>
#include <limits>

namespace PhysX
{
    namespace {
        constexpr int kMaxScenePointerCount = 64;
        constexpr int kMaxActorPointerCount = 65536;
        constexpr int kMaxShapePointerCount = 4096;

        template <typename T>
        inline bool IsArrayLayoutValid(const ATArray<T>& array, int maxCount) {
            return array.count >= 0 && array.max >= 0 && array.count <= array.max &&
                array.count <= maxCount && (array.count == 0 || array.base != 0);
        }

        inline bool ReadPointerArray(uintptr_t base, int count, int maxCount,
                                     std::vector<uint64_t>& out) {
            out.clear();
            if (count == 0) return true;
            if (base == 0 || count < 0 || count > maxCount) return false;

            const size_t byteCount = static_cast<size_t>(count) * sizeof(uint64_t);
            if (base > std::numeric_limits<uintptr_t>::max() - (byteCount - 1)) {
                return false;
            }

            out.resize(static_cast<size_t>(count));
            vm_readv(static_cast<long int>(base), out.data(), byteCount);
            return true;
        }
    }

    inline std::vector<PrunerPayload> CollectRigidActorShapes(ActorType targetType) {
        std::vector<PrunerPayload> result;

        try {
            ATArray<uint64_t> Physx_scenes = vm_read<ATArray<uint64_t>>(Data.PhysxInstancePtr + 8);
            if (!IsArrayLayoutValid(Physx_scenes, kMaxScenePointerCount)) {
                return result;
            }

            std::vector<uint64_t> scenePointers;
            if (!ReadPointerArray(Physx_scenes.base, Physx_scenes.count,
                                  kMaxScenePointerCount, scenePointers)) {
                return result;
            }

            std::vector<uint64_t> actorPointers;
            std::vector<uint64_t> shapePointers;
            for (uint64_t Scene_ptr : scenePointers) {
                if (Scene_ptr == 0) continue;

                ATArray<uint64_t> Actors = vm_read<ATArray<uint64_t>>(Scene_ptr + 9704); // RigidActors 0x2568
                if (!IsArrayLayoutValid(Actors, kMaxActorPointerCount) ||
                    !ReadPointerArray(Actors.base, Actors.count,
                                      kMaxActorPointerCount, actorPointers)) {
                    continue;
                }
                result.reserve(result.size() + actorPointers.size());

                for (uint64_t Actor_ptr : actorPointers) {
                    if (Actor_ptr == 0) continue;

                    unsigned char actorHeader[42]{};
                    vm_readv(static_cast<long int>(Actor_ptr + 8), actorHeader,
                             sizeof(actorHeader));
                    uint16_t Actor_type = 0;
                    uint64_t shapeStorage = 0;
                    uint16_t shapeCount = 0;
                    std::memcpy(&Actor_type, actorHeader, sizeof(Actor_type));
                    std::memcpy(&shapeStorage, actorHeader + 32, sizeof(shapeStorage));
                    std::memcpy(&shapeCount, actorHeader + 40, sizeof(shapeCount));

//                    std::cout << "Actor type: " << Actor_type << " at " << std::hex << Actor_ptr << std::dec << std::endl;

                    if (Actor_type != static_cast<uint16_t>(targetType)) {
                        continue;
                    }

                    if (shapeCount == 1) {
                        if (shapeStorage != 0) {
                            result.push_back({shapeStorage, Actor_ptr});
                        }
                    }
                    else if (shapeCount > 1) {
                        if (ReadPointerArray(shapeStorage, shapeCount,
                                             kMaxShapePointerCount, shapePointers)) {
                            for (uint64_t shapePtr : shapePointers) {
                                if (shapePtr != 0) {
                                    result.push_back({shapePtr, Actor_ptr});
                                }
                            }
                        }
                    }
                }
            }
        }
        catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
        }

        return result;
    }

	inline std::vector<PrunerPayload> CollectAllDynamicRigidActorShapes() {
		return CollectRigidActorShapes(ActorType::DYNAMIC);
	}

	inline std::vector<PrunerPayload> CollectAllHeightRigidActorShapes() {
		return CollectRigidActorShapes(ActorType::STATIC);
	}

	inline std::vector<PrunerPayload> CollectAllStaticRigidActorShapes() {
		return CollectRigidActorShapes(ActorType::STATIC);
	}

	inline std::vector<TriangleMeshData> LoadDynamicRigidShape(std::set<PrunerPayload>& currentSceneObjects, std::unordered_map<PrunerPayload, physx::PxTransform, PrunerPayloadHash>& cache, std::unordered_map<PrunerPayload, uint64_t, PrunerPayloadHash>& ptrCache, std::set<PrunerPayload>& willRemoveObjects) {

        std::vector<PrunerPayload> mObjects = CollectAllDynamicRigidActorShapes();
        std::set<uint64_t> queryActor{};

        for (const auto& obj : mObjects) {
            queryActor.insert(obj.Actor);
        }

        std::unordered_map<uint64_t, BodyT> actorPos;
        std::unordered_map<uint64_t, ShapeT> shapePos;
        actorPos.reserve(queryActor.size());
        shapePos.reserve(mObjects.size());

        for (const auto& actor : queryActor) {
            BodyT body;
            vm_readv(actor, &body, sizeof(BodyT));
            actorPos[actor] = body;
        }

        for (const auto& obj : mObjects) {
            ShapeT shape;
            vm_readv(obj.Shape, &shape, sizeof(ShapeT));
            shapePos[obj.Shape] = shape;
        }

		std::set<PrunerPayload> removeObjects{};
		std::vector<PrunerPayload> addObjects{};

		for (const auto& obj : mObjects) {
			auto& body = actorPos[obj.Actor];
			auto& shape = shapePos[obj.Shape];
			physx::PxTransform GlobalPose = (body.mRigid.mCore.mBodyToWorld * body.mRigid.mBodyToActor.getInverse()) * shape.ShapeCore.mCore.transform;

            auto it = cache.find(obj);
            if (it != cache.end()) {
                const float positionTolerance = 0.01f;
                const float rotationTolerance = 0.99f;

                const physx::PxTransform& a = it->second;
                const physx::PxTransform& b = GlobalPose;

                bool positionChanged = (a.p - b.p).magnitudeSquared() > (positionTolerance * positionTolerance);

                float dot = a.q.x * b.q.x + a.q.y * b.q.y + a.q.z * b.q.z + a.q.w * b.q.w;
                bool rotationChanged = std::abs(dot) < rotationTolerance;

                if (positionChanged || rotationChanged) {
                    currentSceneObjects.erase(obj);
                    willRemoveObjects.insert(obj);
                }
            }
			cache[obj] = GlobalPose;

			auto ptrIt = ptrCache.find(obj);
			auto trianglePtr = (uint64_t)((PxTriangleMeshGeometryT&)shape.GetGeometry()).mTriangleMesh;
			if (ptrIt != ptrCache.end()) {
				if (ptrIt->second != trianglePtr) {
					currentSceneObjects.erase(obj);
					willRemoveObjects.insert(obj);
				}
			}
			ptrCache[obj] = trianglePtr;
		}

		std::set<PrunerPayload> mObjectsSet(mObjects.begin(), mObjects.end());
		for (auto it = cache.begin(); it != cache.end(); ) {
			if (mObjectsSet.find(it->first) == mObjectsSet.end()) {
				it = cache.erase(it);
			}
			else {
				++it;
			}
		}

		for (auto it = ptrCache.begin(); it != ptrCache.end(); ) {
			if (mObjectsSet.find(it->first) == mObjectsSet.end()) {
				it = ptrCache.erase(it);
			}
			else {
				++it;
			}
		}

		std::set<PrunerPayload> result;
		for (const auto& [obj, pos] : cache) {
			result.insert(obj);
		}

		std::copy_if(currentSceneObjects.begin(), currentSceneObjects.end(), std::inserter(removeObjects, removeObjects.begin()), [&result](const PrunerPayload& obj) {
			return result.find(obj) == result.end();
        });

		std::copy_if(result.begin(), result.end(), std::back_inserter(addObjects), [&currentSceneObjects](const PrunerPayload& obj) {
			return currentSceneObjects.find(obj) == currentSceneObjects.end();
        });

		for (const auto& obj : removeObjects) {
			currentSceneObjects.erase(obj);
			willRemoveObjects.insert(obj);
		}

		for (const auto& obj : addObjects) {
			currentSceneObjects.insert(obj);
		}

        return GetMeshDataOptimized(addObjects, [](const ShapeDataT& shape_data) {
            PxGeometryType type = shape_data.mType;
            uint8_t Flags = shape_data.m_shape_flags;
            return !((
            type == PxGeometryType::eTRIANGLEMESH ||
//            type == PxGeometryType::eBOX ||
//            type == PxGeometryType::eCONVEXMESH ||
//            type == PxGeometryType::eCAPSULE ||
            (type == PxGeometryType::eSPHERE && (Flags & 0x01) && !(Flags & 0x04)) // 仅保留物理实体球体（投掷物/载具），排除触发器球体
            ) && shape_data.Shape.ShapeCore.mCore.materialIndex == 0);
            },true
        );
	}

	inline std::vector<TriangleMeshData> RefreshDynamicLoadHeightField(uint32_t& lastTimestamp, std::set<PrunerPayload>& UniqueKeySet, std::set<PrunerPayload>& HeightFieldSet, std::set<uint64_t>& HeightFieldSamplePtrSet, std::set<uint64_t>& RemoveHeightFieldKey) {

		std::vector<PrunerPayload> mObjects = CollectAllHeightRigidActorShapes();

		std::set<PrunerPayload> newUnionKeySet(mObjects.begin(), mObjects.end());
		std::vector<PrunerPayload> newObjects;

		std::copy_if(mObjects.begin(), mObjects.end(), std::back_inserter(newObjects), [&UniqueKeySet, &HeightFieldSet](const PrunerPayload& obj) {
			return UniqueKeySet.find(obj) == UniqueKeySet.end() || HeightFieldSet.find(obj) != HeightFieldSet.end();
        });

		UniqueKeySet.insert(newObjects.begin(), newObjects.end());

		std::set<PrunerPayload> RemoveKey{};
		std::set_difference(UniqueKeySet.begin(), UniqueKeySet.end(), newUnionKeySet.begin(), newUnionKeySet.end(), std::inserter(RemoveKey, RemoveKey.begin()));

		for (const auto& key : RemoveKey) {
			UniqueKeySet.erase(key);
		}

		std::set<uint64_t> currentHeightFieldPtrSet{};
		auto result = GetMeshDataOptimized(newObjects, [&HeightFieldSamplePtrSet, &currentHeightFieldPtrSet, &HeightFieldSet](const ShapeDataT& shape_data) {
			PxGeometryType type = shape_data.mType;
			if (type == PxGeometryType::eHEIGHTFIELD) {
				PxHeightFieldGeometryT field_geometry = (PxHeightFieldGeometryT&)shape_data.Shape.GetGeometry();
				HeightFieldSet.insert(shape_data.UniqueKey);
				if (field_geometry.RowScale == 200.0f) {
					auto ptr = (uint64_t)field_geometry.mHeightField;
					currentHeightFieldPtrSet.insert(ptr);
					bool alreadyExists = !HeightFieldSamplePtrSet.insert(ptr).second;
					return alreadyExists;
				}
				return true;
			}
			return true;
        });

		std::set_difference(HeightFieldSamplePtrSet.begin(), HeightFieldSamplePtrSet.end(), currentHeightFieldPtrSet.begin(), currentHeightFieldPtrSet.end(), std::inserter(RemoveHeightFieldKey, RemoveHeightFieldKey.begin()));

		for (const auto& key : RemoveHeightFieldKey) {
			HeightFieldSamplePtrSet.erase(key);
		}

		return result;
	}

	inline std::vector<TriangleMeshData> LoadShapeByRange(uint32_t& lastTimestamp, std::unordered_map<PrunerPayload, physx::PxTransform, PrunerPayloadHash>& cache, std::set<PrunerPayload>& currentSceneObjects, std::set<PrunerPayload>& willRemoveObjects, std::unordered_map<PrunerPayload, uint64_t, PrunerPayloadHash>& alwaysCheckShape) {

		std::vector<PrunerPayload> mObjects = CollectAllStaticRigidActorShapes();
		std::set<PrunerPayload> queryObjects{};
		std::set<PrunerPayload> mObjectsSet(mObjects.begin(), mObjects.end());

		for (const auto& obj : mObjectsSet) {
			if (cache.find(obj) == cache.end()) {
				queryObjects.insert(obj);
			}
		}

		std::set<uint64_t> queryActor{};
		for (const auto& obj : queryObjects) {
			queryActor.insert(obj.Actor);
		}

		std::unordered_map<uint64_t, BodyT> actorPos{};
		std::unordered_map<uint64_t, ShapeT> shapePos{};
		actorPos.reserve(queryActor.size());
		shapePos.reserve(queryObjects.size());

		for (const auto& actor : queryActor) {
			actorPos[actor] = {};
		}

		for (const auto& obj : queryObjects) {
			shapePos[obj.Shape] = {};
		}

		for (const auto& obj : queryObjects) {
			auto& body = actorPos[obj.Actor];
			auto& shape = shapePos[obj.Shape];
			physx::PxTransform GlobalPose = body.mRigid.mCore.mBodyToWorld * shape.ShapeCore.mCore.transform;
			cache[obj] = GlobalPose;
		}

		for (auto it = cache.begin(); it != cache.end(); ) {
			if (mObjectsSet.find(it->first) == mObjectsSet.end()) {
				it = cache.erase(it);
			}
			else {
				++it;
			}
		}

		std::set<PrunerPayload> removeObjects{};
		std::vector<PrunerPayload> addObjects{};
		std::set<PrunerPayload> result;

		for (const auto& [obj, pos] : cache) {
			result.insert(obj);
		}

		std::copy_if(currentSceneObjects.begin(), currentSceneObjects.end(), std::inserter(removeObjects, removeObjects.begin()), [&result](const PrunerPayload& obj) {
			return result.find(obj) == result.end();
        });

		std::copy_if(result.begin(), result.end(), std::back_inserter(addObjects), [&currentSceneObjects, &alwaysCheckShape](const PrunerPayload& obj) {
			return currentSceneObjects.find(obj) == currentSceneObjects.end() || alwaysCheckShape.find(obj) != alwaysCheckShape.end();
        });

		for (const auto& obj : removeObjects) {
			currentSceneObjects.erase(obj);
			alwaysCheckShape.erase(obj);
			willRemoveObjects.insert(obj);
		}

		for (const auto& obj : addObjects) {
			currentSceneObjects.insert(obj);
		}

		auto res = GetMeshDataOptimized(addObjects, [&alwaysCheckShape, &willRemoveObjects, &currentSceneObjects](const ShapeDataT& shape_data) {
			PxGeometryType type = shape_data.mType;
			uint8_t Flags = shape_data.m_shape_flags;
            if (type != PxGeometryType::eTRIANGLEMESH && type != PxGeometryType::eBOX && type != PxGeometryType::eCONVEXMESH && type != PxGeometryType::eSPHERE) {
				willRemoveObjects.insert(shape_data.UniqueKey);
				currentSceneObjects.erase(shape_data.UniqueKey);
				return true;
			}

			auto findRes = alwaysCheckShape.find(shape_data.UniqueKey);
			auto trianglePtr = (uint64_t)((PxTriangleMeshGeometryT&)shape_data.Shape.GetGeometry()).mTriangleMesh;

			if (findRes == alwaysCheckShape.end()) {
				alwaysCheckShape.insert({ shape_data.UniqueKey, trianglePtr });
			}
			else {
				if (findRes->second == trianglePtr) {
					return true;
				}
				else {
					alwaysCheckShape[shape_data.UniqueKey] = trianglePtr;
					willRemoveObjects.insert(shape_data.UniqueKey);
					currentSceneObjects.erase(shape_data.UniqueKey);
					return false;
				}
			}

            bool shouldInclude = ((type == PxGeometryType::eTRIANGLEMESH) && shape_data.Shape.ShapeCore.mCore.materialIndex != 0);

            return !shouldInclude;
			}
		);

		return res;
	}
}

#endif
