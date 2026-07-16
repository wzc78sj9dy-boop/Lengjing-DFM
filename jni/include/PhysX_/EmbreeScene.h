//
// Created by binglen on 2025/6/8.
//

#ifndef EMBREE_SCENE_H
#define EMBREE_SCENE_H

#include "PhysXStructures.h"
#include "PhysXMeshData.h"
#include "rtcore.h"
#include "rtcore_ray.h"
#include <shared_mutex>
#include <atomic>
#include <memory>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <limits>
#include <deque>
#include <vector>
#include <set>
#include <unordered_map>

namespace Physics {

	template <typename T>
	struct BatchedUpdate {
		std::vector<TriangleMeshData> meshesToAdd;
		std::set<T> keysToRemove;
		std::vector<std::shared_ptr<TriangleMeshData>> processedMeshes;

		void Clear() {
			meshesToAdd.clear();
			keysToRemove.clear();
			processedMeshes.clear();
		}

		bool Empty() const {
			return meshesToAdd.empty() && keysToRemove.empty();
		}
	};

	static void embreeErrorFunction(void* userPtr, RTCError code, const char* str) {
		const char* errorType = "未知";
		switch (code) {
		case RTC_ERROR_NONE: errorType = "无错误"; break;
		case RTC_ERROR_UNKNOWN: errorType = "未知错误"; break;
		case RTC_ERROR_INVALID_ARGUMENT: errorType = "无效参数"; break;
		case RTC_ERROR_INVALID_OPERATION: errorType = "无效操作"; break;
		case RTC_ERROR_OUT_OF_MEMORY: errorType = "内存不足"; break;
		case RTC_ERROR_UNSUPPORTED_CPU: errorType = "不支持的CPU"; break;
		case RTC_ERROR_CANCELLED: errorType = "已取消"; break;
		}
        EMBREE_ERROR( "Embree错误 [%s]: %s\n" , errorType, str ? str : "无详细信息");
	}

	template <typename T, typename Hash>
	class VisibleScene {
	private:
		struct GeometryInfo {
			uint32_t geomId;
			std::shared_ptr<TriangleMeshData> meshData;
			std::atomic<bool> inUse;

			GeometryInfo(uint32_t id, std::shared_ptr<TriangleMeshData> data, bool use)
				: geomId(id), meshData(std::move(data)), inUse(use) {
			}

			GeometryInfo(const GeometryInfo&) = delete;
			GeometryInfo& operator=(const GeometryInfo&) = delete;

			GeometryInfo(GeometryInfo&& other) noexcept
				: geomId(other.geomId)
				, meshData(std::move(other.meshData))
				, inUse(other.inUse.load()) {
			}

			GeometryInfo& operator=(GeometryInfo&& other) noexcept {
				if (this != &other) {
					geomId = other.geomId;
					meshData = std::move(other.meshData);
					inUse.store(other.inUse.load());
				}
				return *this;
			}
		};

		std::atomic<bool> updateInProgress{ false };
		std::shared_ptr<BatchedUpdate<T>> pendingUpdate;
		std::mutex updateQueueMutex;

		std::unordered_map<T, std::unique_ptr<GeometryInfo>, Hash> geometry_info_map;
		mutable std::shared_mutex geometry_mutex;

	public:
		using KeyExtractor = T(*)(const TriangleMeshData&);

		VisibleScene(KeyExtractor keyExtractor) : getKey(keyExtractor) {
			InitializeEmbree();
		}

		~VisibleScene() {
			SafeDestroy();
		}

		bool HealthCheck() {
			std::shared_lock<std::shared_mutex> lock(sceneMutex);

			if (!device || !scene) {
				return false;
			}

			RTCError error = rtcGetDeviceError(device);
			if (error != RTC_ERROR_NONE) {
				return false;
			}

			return true;
		}

		void EmergencyRecovery() {

			updateInProgress.store(true, std::memory_order_release);

			SafeDestroy();

			InitializeEmbree();

			updateInProgress.store(false, std::memory_order_release);
		}

		void QueueUpdate(const std::vector<TriangleMeshData>& willAddMeshs, const std::set<T>& removeKeys) {
			if (willAddMeshs.empty() && removeKeys.empty()) {
				return;
			}

			std::lock_guard<std::mutex> lock(updateQueueMutex);
			if (!pendingUpdate) {
				pendingUpdate = std::make_shared<BatchedUpdate<T>>();
			}

			pendingUpdate->meshesToAdd.insert(pendingUpdate->meshesToAdd.end(), willAddMeshs.begin(), willAddMeshs.end());
			pendingUpdate->keysToRemove.insert(removeKeys.begin(), removeKeys.end());
		}

		bool ProcessPendingUpdates(bool force = false) {
			bool expected = false;
			if (!updateInProgress.compare_exchange_strong(expected, true) && !force) {
				return false;
			}

			std::shared_ptr<BatchedUpdate<T>> currentUpdate;
			{
				std::lock_guard<std::mutex> lock(updateQueueMutex);
				if (!pendingUpdate || pendingUpdate->Empty()) {
					updateInProgress.store(false, std::memory_order_release);
					return false;
				}
				currentUpdate = std::move(pendingUpdate);
			}

			bool success = false;
			try {
				PreprocessBatchData(*currentUpdate);
				ApplyBatchUpdate(*currentUpdate);
				currentUpdate->Clear();
				success = true;
			}
			catch (const std::exception& e) {
				std::lock_guard<std::mutex> lock(updateQueueMutex);
				if (!pendingUpdate) {
					pendingUpdate = currentUpdate;
				}
			}

			updateInProgress.store(false, std::memory_order_release);
			return success;
		}

        RTCRayHit Raycast(physx::PxVec3& origin, physx::PxVec3& target) {
            RTCRayHit rayhit;
            memset(&rayhit, 0, sizeof(RTCRayHit));

            if (!ValidateRayInput(origin, target)) {
                rayhit.ray.tfar = -1.0f;
                rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
                return rayhit;
            }

            // try_lock: 场景更新中（BVH重建）时不阻塞渲染线程，直接返回未命中
            std::shared_lock<std::shared_mutex> lock(sceneMutex, std::try_to_lock);
            if (!lock.owns_lock()) {
                rayhit.ray.tfar = -1.0f;
                rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
                return rayhit;
            }

            if (!scene || !device) {
                rayhit.ray.tfar = -1.0f;
                rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
                return rayhit;
            }

            SetupRay(rayhit, origin, target);

            if (!std::isfinite(rayhit.ray.dir_x) || !std::isfinite(rayhit.ray.dir_y) || !std::isfinite(rayhit.ray.dir_z) ||
                rayhit.ray.tnear < 0.0f || rayhit.ray.tfar < 0.0f || rayhit.ray.tnear > rayhit.ray.tfar) {
                rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
                return rayhit;
            }

            try {
                RTCIntersectArguments args;
                rtcInitIntersectArguments(&args);
                rtcIntersect1(scene, &rayhit, &args);
            }
            catch (const std::exception& e) {
                rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
            }

            return rayhit;
        }

        // 高性能遮挡检测：使用 rtcOccluded1 代替 rtcIntersect1
        // rtcOccluded1 只判断射线是否被遮挡，无需计算交点细节，比 rtcIntersect1 快得多
        // 返回 true 表示射线被遮挡（命中了物体）
        bool OcclusionTest(const physx::PxVec3& origin, const physx::PxVec3& target) {
            if (!ValidateRayInput(origin, target)) {
                return false;
            }

            // try_lock: 场景更新中时不阻塞，假设未遮挡
            std::shared_lock<std::shared_mutex> lock(sceneMutex, std::try_to_lock);
            if (!lock.owns_lock()) {
                return false;
            }

            if (!scene || !device) {
                return false;
            }

            RTCRay ray;
            memset(&ray, 0, sizeof(RTCRay));

            ray.org_x = origin.x;
            ray.org_y = origin.y;
            ray.org_z = origin.z;

            physx::PxVec3 direction = target - origin;
            float dir_length = direction.magnitude();
            if (dir_length < 1e-6f) return false;

            direction = direction.getNormalized();
            if (!std::isfinite(direction.x) || !std::isfinite(direction.y) || !std::isfinite(direction.z)) {
                return false;
            }

            ray.dir_x = direction.x;
            ray.dir_y = direction.y;
            ray.dir_z = direction.z;
            ray.tnear = 0.0f;
            ray.tfar = dir_length;
            ray.mask = -1;
            ray.flags = 0;

            try {
                RTCOccludedArguments args;
                rtcInitOccludedArguments(&args);
                rtcOccluded1(scene, &ray, &args);
            }
            catch (const std::exception& e) {
                return false;
            }

            // rtcOccluded1 命中时将 tfar 设为 -INFINITY
            return ray.tfar < 0.0f;
        }

		TriangleMeshData* GetGeometryData(uint32_t geomId) {
			std::shared_lock<std::shared_mutex> lock(sceneMutex, std::try_to_lock);
			if (!lock.owns_lock()) {
				return nullptr;
			}

			if (!scene || !device) {
				return nullptr;
			}

			try {
				RTCGeometry geom = rtcGetGeometry(scene, geomId);
				if (geom) {
					return static_cast<TriangleMeshData*>(rtcGetGeometryUserData(geom));
				}
			}
			catch (const std::exception& e) {
				printf(COLOR_RED "[?] 获取几何数据异常: %s\n" COLOR_RESET, e.what());
			}

			return nullptr;
		}

        std::vector<TriangleMeshData*> GetNearMesh(const FVector& Position, double RadiusSquare) const {
            std::vector<TriangleMeshData*> result;
            std::shared_lock<std::shared_mutex> lock(geometry_mutex, std::try_to_lock);
            if (!lock.owns_lock()) {
                return result;
            }
            for (const auto& pair : geometry_info_map) {
                const auto& geomInfo = pair.second;
                if (geomInfo && geomInfo->meshData) {
                    auto meshCenter = geomInfo->meshData->Transform.p;
                    physx::PxVec3 position = { Position.X, Position.Y, Position.Z };
                    auto distance = (meshCenter - position).magnitudeSquared();
                    if (distance <= RadiusSquare) {
                        result.push_back(geomInfo->meshData.get());
                    }
                }
            }
            return result;
        }

		bool isValid() const {
			return initialized && device != nullptr && scene != nullptr;
		}

//		size_t GetGeometryCount() const {
//			std::shared_lock<std::shared_mutex> lock(geometry_mutex);
//			return geometry_info_map.size();
//		}

		bool HasPendingUpdates() const {
			return pendingUpdate && !pendingUpdate->Empty();
		}

	private:
		void InitializeEmbree() {
			try {

				// 单射线查询和低频场景提交无需常驻多个 Embree worker。
				device = rtcNewDevice("threads=1");
				if (!device) {
					throw std::runtime_error("[x] 创建 Embree 设备失败");
				}

				rtcSetDeviceErrorFunction(device, embreeErrorFunction, nullptr);

				scene = rtcNewScene(device);
				if (!scene) {
					throw std::runtime_error("[x] 创建 Embree 场景失败");
				}

                rtcSetSceneBuildQuality(scene, RTC_BUILD_QUALITY_LOW);

                rtcSetSceneFlags(scene, RTC_SCENE_FLAG_DYNAMIC | RTC_SCENE_FLAG_COMPACT);

				rtcCommitScene(scene);

				RTCError error = rtcGetDeviceError(device);
				if (error != RTC_ERROR_NONE) {
					throw std::runtime_error("[x] Embree设备初始化后出现错误");
				}

				initialized = true;

			}
			catch (const std::exception& e) {
				SafeDestroy();
				throw;
			}
		}

		void SafeDestroy() {
			std::unique_lock<std::shared_mutex> sceneLock(sceneMutex);
			std::unique_lock<std::shared_mutex> geometryLock(geometry_mutex);

			if (scene) {
				for (auto& [key, info] : geometry_info_map) {
					if (info && info->inUse.load()) {
						RTCGeometry geom = rtcGetGeometry(scene, info->geomId);
						if (geom) {
							rtcDetachGeometry(scene, info->geomId);
						}
					}
				}
				geometry_info_map.clear();

				rtcReleaseScene(scene);
				scene = nullptr;
			}

			if (device) {
				rtcReleaseDevice(device);
				device = nullptr;
			}

			initialized = false;
		}

		bool ValidateRayInput(const physx::PxVec3& origin, const physx::PxVec3& target) {
			return std::isfinite(origin.x) && std::isfinite(origin.y) && std::isfinite(origin.z) && std::isfinite(target.x) && std::isfinite(target.y) && std::isfinite(target.z);
		}

        void SetupRay(RTCRayHit& rayhit, const physx::PxVec3& origin, const physx::PxVec3& target) {

            memset(&rayhit, 0, sizeof(RTCRayHit));

            rayhit.ray.org_x = origin.x;
            rayhit.ray.org_y = origin.y;
            rayhit.ray.org_z = origin.z;

            physx::PxVec3 direction = target - origin;
            float dir_length = direction.magnitude();

            if (dir_length < 1e-6f) {
                rayhit.ray.tnear = 0.0f;
                rayhit.ray.tfar = -std::numeric_limits<float>::max();
                rayhit.ray.dir_x = 0.0f;
                rayhit.ray.dir_y = 0.0f;
                rayhit.ray.dir_z = 0.0f;
                rayhit.ray.mask = 0;
                rayhit.ray.flags = 0;
                rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
                rayhit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
                return;
            }

            direction = direction.getNormalized();

            if (!std::isfinite(direction.x) || !std::isfinite(direction.y) || !std::isfinite(direction.z)) {
                rayhit.ray.tfar = -std::numeric_limits<float>::max();
                rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
                return;
            }

            rayhit.ray.dir_x = direction.x;
            rayhit.ray.dir_y = direction.y;
            rayhit.ray.dir_z = direction.z;
            rayhit.ray.tnear = 0.0f;
            rayhit.ray.tfar = dir_length;
            rayhit.ray.mask = -1;
            rayhit.ray.flags = 0;

            rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
            rayhit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
            rayhit.hit.primID = RTC_INVALID_GEOMETRY_ID;
        }

		void PreprocessBatchData(BatchedUpdate<T>& batch) {
			batch.processedMeshes.clear();
			batch.processedMeshes.reserve(batch.meshesToAdd.size());

			for (auto& mesh : batch.meshesToAdd) {
				if (mesh.Vertices.empty() || mesh.Indices.empty()) {
					continue;
				}

				TriangleMeshData processed_mesh = mesh;

				if (!PreprocessMeshData(processed_mesh)) {
					continue;
				}

				if (!ValidateMeshDataStrict(processed_mesh)) {
					continue;
				}

				T key = getKey(processed_mesh);
				if constexpr (std::is_same_v<T, PrunerPayload>) {
					if (key.Shape == 0 || key.Actor == 0) {
						continue;
					}
				}

				auto mesh_copy = std::make_shared<TriangleMeshData>();
				*mesh_copy = std::move(processed_mesh);
				batch.processedMeshes.push_back(std::move(mesh_copy));
			}
		}

		void ApplyBatchUpdate(BatchedUpdate<T>& batch) {
			if (!isValid()) {
				return;
			}

			// Phase 1: 无锁阶段 - 创建和准备所有新几何体
			struct PreparedGeometry {
				RTCGeometry geom;
				std::shared_ptr<TriangleMeshData> meshData;
				T key;
			};
			std::vector<PreparedGeometry> preparedGeoms;
			preparedGeoms.reserve(batch.processedMeshes.size());

			for (auto& mesh_ptr : batch.processedMeshes) {
				if (!mesh_ptr || mesh_ptr->Vertices.empty() || mesh_ptr->Indices.empty()) {
					continue;
				}

				if (!device) break;

				RTCGeometry geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
				if (!geom) {
					continue;
				}

				float* vertices = static_cast<float*>(rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, 3 * sizeof(float), mesh_ptr->Vertices.size()));
				if (!vertices) {
					rtcReleaseGeometry(geom);
					continue;
				}

				for (size_t j = 0; j < mesh_ptr->Vertices.size(); j++) {
					vertices[j * 3] = mesh_ptr->Vertices[j].x;
					vertices[j * 3 + 1] = mesh_ptr->Vertices[j].y;
					vertices[j * 3 + 2] = mesh_ptr->Vertices[j].z;
				}

				unsigned int* indices = static_cast<unsigned int*>(rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, 3 * sizeof(unsigned int), mesh_ptr->Indices.size() / 3));
				if (!indices) {
					rtcReleaseGeometry(geom);
					continue;
				}

				memcpy(indices, mesh_ptr->Indices.data(), mesh_ptr->Indices.size() * sizeof(unsigned int));

				rtcSetGeometryUserData(geom, mesh_ptr.get());
				rtcCommitGeometry(geom);

				T key = getKey(*mesh_ptr);
				preparedGeoms.push_back({geom, mesh_ptr, key});
			}

			// Phase 2: 持锁阶段 - 仅做快速的attach/detach + commit
			{
				std::unique_lock<std::shared_mutex> sceneLock(sceneMutex);
				std::unique_lock<std::shared_mutex> geometryLock(geometry_mutex);

				if (!device || !scene) {
					for (auto& pg : preparedGeoms) {
						rtcReleaseGeometry(pg.geom);
					}
					return;
				}

				for (auto& key : batch.keysToRemove) {
					auto it = geometry_info_map.find(key);
					if (it != geometry_info_map.end()) {
						if (it->second && it->second->inUse.load()) {
							RTCGeometry geom = rtcGetGeometry(scene, it->second->geomId);
							if (geom) {
								rtcDetachGeometry(scene, it->second->geomId);
							}
						}
						geometry_info_map.erase(it);
					}
				}

				for (auto& pg : preparedGeoms) {
					auto existing_it = geometry_info_map.find(pg.key);
					if (existing_it != geometry_info_map.end()) {
						if (existing_it->second && existing_it->second->inUse.load()) {
							RTCGeometry geom = rtcGetGeometry(scene, existing_it->second->geomId);
							if (geom) {
								rtcDetachGeometry(scene, existing_it->second->geomId);
							}
						}
						geometry_info_map.erase(existing_it);
					}

					uint32_t geometry_id = rtcAttachGeometry(scene, pg.geom);
					rtcReleaseGeometry(pg.geom);

					if (geometry_id == RTC_INVALID_GEOMETRY_ID) {
						continue;
					}

					geometry_info_map.emplace(pg.key, std::make_unique<GeometryInfo>(geometry_id, pg.meshData, true));
				}

				rtcCommitScene(scene);
			}
		}

		bool PreprocessMeshData(TriangleMeshData& mesh) {
			for (auto& vertex : mesh.Vertices) {
				if (!std::isfinite(vertex.x)) vertex.x = 0.0f;
				if (!std::isfinite(vertex.y)) vertex.y = 0.0f;
				if (!std::isfinite(vertex.z)) vertex.z = 0.0f;
			}

			std::vector<uint32_t> validIndices;
			validIndices.reserve(mesh.Indices.size());

			for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3) {
				uint32_t i0 = mesh.Indices[i];
				uint32_t i1 = mesh.Indices[i + 1];
				uint32_t i2 = mesh.Indices[i + 2];

				if (i0 >= mesh.Vertices.size() || i1 >= mesh.Vertices.size() || i2 >= mesh.Vertices.size()) {
					continue;
				}

				if (i0 == i1 || i1 == i2 || i2 == i0) {
					continue;
				}

				const auto& v0 = mesh.Vertices[i0];
				const auto& v1 = mesh.Vertices[i1];
				const auto& v2 = mesh.Vertices[i2];

				physx::PxVec3 edge1 = v1 - v0;
				physx::PxVec3 edge2 = v2 - v0;
				physx::PxVec3 normal = edge1.cross(edge2);
				float areaSquared = normal.magnitudeSquared();

				if (areaSquared < 1e-10f) {
					continue;
				}

				validIndices.push_back(i0);
				validIndices.push_back(i1);
				validIndices.push_back(i2);
			}

			if (validIndices.size() < 3) {
				return false;
			}

			mesh.Indices = std::move(validIndices);
			return true;
		}

		bool ValidateMeshDataStrict(const TriangleMeshData& mesh) {
			if (mesh.Vertices.empty() || mesh.Indices.empty()) {
				return false;
			}

			const size_t MAX_VERTICES = 5000000;
			const size_t MAX_INDICES = 15000000;

			if (mesh.Vertices.size() > MAX_VERTICES || mesh.Indices.size() > MAX_INDICES) {
				return false;
			}

			for (const auto& vertex : mesh.Vertices) {
				if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z)) {
					return false;
				}
			}

			for (size_t i = 0; i < mesh.Indices.size(); i++) {
				if (mesh.Indices[i] >= mesh.Vertices.size()) {
					return false;
				}
			}

			return true;
		}

	private:
		std::atomic<bool> initialized{ false };
		RTCDevice device{ nullptr };
		RTCScene scene{ nullptr };
		KeyExtractor getKey;
		mutable std::shared_mutex sceneMutex;
	};

	// 使用函数而不是 lambda 来避免模板实例化问题
	inline PrunerPayload prunerPayloadExtractor(const TriangleMeshData& mesh) {
		return mesh.UniqueKey1;
	}

	inline uint64_t int64Extractor(const TriangleMeshData& mesh) {
		return mesh.UniqueKey2;
	}
}

#endif
