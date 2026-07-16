//
// Created by binglen on 2025/6/8.
//

#ifndef PHYSX_CORE_H
#define PHYSX_CORE_H

#include "PhysXTypes.h"
#include "PhysXEnums.h"
#include "PhysXStructures.h"
#include "PhysXMeshData.h"
#include "PhysXLoader.h"
#include "PhysXCollector.h"
#include "EmbreeScene.h"
#include "PhysX_Log.h"

#include <sys/resource.h>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <functional>
#include <pthread.h>
#include <mutex>
#include <memory>
#include <atomic>
#include <vector>
#include <set>

static std::shared_ptr<Physics::VisibleScene<PrunerPayload, PrunerPayloadHash>> DynamicLoadScene = nullptr;
static std::shared_ptr<Physics::VisibleScene<PrunerPayload, PrunerPayloadHash>> DynamicRigidScene = nullptr;
static std::shared_ptr<Physics::VisibleScene<uint64_t, Int64Hash>> HeightFieldScene = nullptr;

static std::mutex g_dynamic_load_scene_mutex;
static std::mutex g_dynamic_rigid_scene_mutex;
static std::mutex g_height_field_scene_mutex;

static std::atomic<bool> g_shutdown_requested{ false };

static std::unordered_set<size_t> exportedHashes;

class EnhancedThrottler {
private:
	std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastExecuted_;
	std::unordered_map<std::string, int> skipCounters_;

public:
	EnhancedThrottler() = default;

	void executeTaskWithSleep(const std::string& taskName,
		std::chrono::duration<double> interval,
		const std::function<void()>& task,
		int skipFrames = 0) {
		using Clock = std::chrono::steady_clock;
		auto now = Clock::now();

		if (skipFrames > 0) {
			auto& counter = skipCounters_[taskName];
			counter++;
			if (counter <= skipFrames) {
				return;
			}
			counter = 0;
		}

		auto it = lastExecuted_.find(taskName);
		if (it != lastExecuted_.end()) {
			auto elapsed = now - it->second;
			if (elapsed < interval) {
				std::this_thread::sleep_for(interval - elapsed);
			}
		}

		lastExecuted_[taskName] = Clock::now();
		task();
	}

	void reset(const std::string& taskName) {
		lastExecuted_.erase(taskName);
		skipCounters_.erase(taskName);
	}
};

class ThreadStackManager {
public:
	static void ConfigureThreadStack() {
		struct rlimit limit;

		if (getrlimit(RLIMIT_STACK, &limit) == 0) {

			const size_t reasonable_stack = 16 * 1024 * 1024;

			if (limit.rlim_cur < reasonable_stack) {
				limit.rlim_cur = reasonable_stack;
				if (setrlimit(RLIMIT_STACK, &limit) != 0) {
                    CORE_DEBUG( "[-] Failed to set stack size, using system default" );
				}
				else {
                    CORE_DEBUG( "[-] Stack size has been set to: %zu MB" , reasonable_stack / (1024 * 1024));
				}
			}
		}
		else {
            CORE_DEBUG( "[-] Failed to get stack limit" );
		}
	}

	static void SetThreadStackSize(pthread_t thread, size_t stack_size) {

	}
};

class SceneUpdateManager {
private:
	std::string name_;
	std::shared_ptr<EnhancedThrottler> throttler_;
	int consecutive_errors_{ 0 };
	const int max_consecutive_errors_{ 3 };

public:
	SceneUpdateManager(const std::string& name)
		: name_(name), throttler_(std::make_shared<EnhancedThrottler>()) {
	}

	template<typename SceneType, typename UpdateFunction>
	bool ExecuteUpdate(std::shared_ptr<SceneType> scene,
		UpdateFunction update_func,
		std::chrono::milliseconds interval,
		int skip_frames = 0) {
		if (!scene || !scene->isValid()) {
			return false;
		}

		if (!scene->HealthCheck()) {
			consecutive_errors_++;
			if (consecutive_errors_ >= max_consecutive_errors_) {
				scene->EmergencyRecovery();
				consecutive_errors_ = 0;
			}
			return false;
		}
		else {
			consecutive_errors_ = 0;
		}

		throttler_->executeTaskWithSleep(name_, interval, update_func, skip_frames);
		return true;
	}

	void Reset() {
		consecutive_errors_ = 0;
		throttler_->reset(name_);
	}
};

namespace VisibleCheck {
	// 降低PhysX线程优先级，避免CPU争抢导致渲染线程卡顿
	inline void SetLowThreadPriority() {
		setpriority(PRIO_PROCESS, 0, 10);
	}

	inline void* UpdateSceneByRange(void* arg) {
		pthread_setname_np(pthread_self(), "[x] SceneRangeUpdater");
		ThreadStackManager::ConfigureThreadStack();
		SetLowThreadPriority();

		auto cache = std::make_shared<std::unordered_map<PrunerPayload, physx::PxTransform, PrunerPayloadHash>>();
		auto currentSceneObjects = std::make_shared<std::set<PrunerPayload>>();
		auto alwaysCheckShape = std::make_shared<std::unordered_map<PrunerPayload, uint64_t, PrunerPayloadHash>>();
		uint32_t lastUpdateTimestamp = 0;

		SceneUpdateManager update_manager("[x] SceneRangeUpdate");
		auto last_commit_time = std::chrono::steady_clock::now();
		const auto commit_interval = std::chrono::milliseconds(10000);

		while (!g_shutdown_requested.load(std::memory_order_acquire)) {
			try {
				std::shared_ptr<Physics::VisibleScene<PrunerPayload, PrunerPayloadHash>> local_scene;
				{
					std::lock_guard<std::mutex> lock(g_dynamic_load_scene_mutex);
					local_scene = DynamicLoadScene;
				}

				if (!local_scene) {
					std::this_thread::sleep_for(std::chrono::milliseconds(10000));
					continue;
				}

				bool success = update_manager.ExecuteUpdate(local_scene, [&]() {
                    auto willRemoveObjects = std::make_shared<std::set<PrunerPayload>>();
                    auto meshes = PhysX::LoadShapeByRange(lastUpdateTimestamp, *cache, *currentSceneObjects, *willRemoveObjects, *alwaysCheckShape);

                    if (!meshes.empty() || !willRemoveObjects->empty()) {
                        local_scene->QueueUpdate(meshes, *willRemoveObjects);
                    }

                    auto now = std::chrono::steady_clock::now();
                    if (local_scene->HasPendingUpdates() && now - last_commit_time >= commit_interval) {
                        local_scene->ProcessPendingUpdates();
                        last_commit_time = now;
                    }}, std::chrono::milliseconds(10000), 2
				);

				if (!success) {
					std::this_thread::sleep_for(std::chrono::milliseconds(10000));
				}

			}
			catch (const std::exception& e) {
                RAYCAST_DEBUG( "[x] UpdateSceneByRange exception: %s" , e.what());
				std::this_thread::sleep_for(std::chrono::milliseconds(10000));
			}
		}

		return nullptr;
	}

	inline void* UpdateDynamicRigid(void* arg) {

		pthread_setname_np(pthread_self(), "[x] DynamicRigidUpdater");
		ThreadStackManager::ConfigureThreadStack();
		SetLowThreadPriority();

		auto cache = std::make_shared<std::unordered_map<PrunerPayload, physx::PxTransform, PrunerPayloadHash>>();
		auto ptrCache = std::make_shared<std::unordered_map<PrunerPayload, uint64_t, PrunerPayloadHash>>();
		auto currentSceneObjects = std::make_shared<std::set<PrunerPayload>>();

		SceneUpdateManager update_manager("[x] DynamicRigidUpdate");
		auto last_commit_time = std::chrono::steady_clock::now();
		const auto commit_interval = std::chrono::milliseconds(3000);

		while (!g_shutdown_requested.load(std::memory_order_acquire)) {
			try {
				std::shared_ptr<Physics::VisibleScene<PrunerPayload, PrunerPayloadHash>> local_scene;
				{
					std::lock_guard<std::mutex> lock(g_dynamic_rigid_scene_mutex);
					local_scene = DynamicRigidScene;
				}

				if (!local_scene) {
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
					continue;
				}

				bool success = update_manager.ExecuteUpdate(local_scene, [&]() {
                    auto willRemoveShape = std::make_shared<std::set<PrunerPayload>>();
                    auto meshes = PhysX::LoadDynamicRigidShape(*currentSceneObjects, *cache, *ptrCache, *willRemoveShape);

                    if (!meshes.empty() || !willRemoveShape->empty()) {
                        local_scene->QueueUpdate(meshes, *willRemoveShape);
                    }

                    auto now = std::chrono::steady_clock::now();
                    if (local_scene->HasPendingUpdates() && now - last_commit_time >= commit_interval) {
                        local_scene->ProcessPendingUpdates();
                        last_commit_time = now;
                    }}, std::chrono::milliseconds(100), 0
				);

				if (!success) {
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
				}

			}
			catch (const std::exception& e) {
                RAYCAST_DEBUG( "[x] UpdateDynamicRigid exception: %s" , e.what());
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
		}

		return nullptr;
	}

	inline void* UpdateDynamicHeightField(void* arg) {

		pthread_setname_np(pthread_self(), "[x] HeightFieldUpdater");
		ThreadStackManager::ConfigureThreadStack();
		SetLowThreadPriority();

		auto UniqueSet = std::make_shared<std::set<PrunerPayload>>();
		auto HeightFieldSet = std::make_shared<std::set<PrunerPayload>>();
		auto HeightFieldSamplePtrSet = std::make_shared<std::set<uint64_t>>();
		uint32_t lastUpdateTimestamp = 0;

		SceneUpdateManager update_manager("[x] HeightFieldUpdate");
		auto last_commit_time = std::chrono::steady_clock::now();
		const auto commit_interval = std::chrono::milliseconds(20000);

		while (!g_shutdown_requested.load(std::memory_order_acquire)) {
			try {
				std::shared_ptr<Physics::VisibleScene<uint64_t, Int64Hash>> local_scene;
				{
					std::lock_guard<std::mutex> lock(g_height_field_scene_mutex);
					local_scene = HeightFieldScene;
				}

				if (!local_scene) {
					std::this_thread::sleep_for(std::chrono::milliseconds(15000));
					continue;
				}

				bool success = update_manager.ExecuteUpdate(local_scene, [&]() {
                    auto RemoveHeightFieldKey = std::make_shared<std::set<uint64_t>>();
                    auto meshes = PhysX::RefreshDynamicLoadHeightField(lastUpdateTimestamp, *UniqueSet, *HeightFieldSet, *HeightFieldSamplePtrSet, *RemoveHeightFieldKey);

                    if (!meshes.empty() || !RemoveHeightFieldKey->empty()) {
                        local_scene->QueueUpdate(meshes, *RemoveHeightFieldKey);
                    }

                    auto now = std::chrono::steady_clock::now();
                    if (local_scene->HasPendingUpdates() && now - last_commit_time >= commit_interval) {
                        local_scene->ProcessPendingUpdates();
                        last_commit_time = now;
                    }}, std::chrono::milliseconds(15000), 15
				);

				if (!success) {
					std::this_thread::sleep_for(std::chrono::milliseconds(15000));
				}

			}
			catch (const std::exception& e) {
                RAYCAST_DEBUG( "[x] UpdateDynamicHeightField exception: %s" , e.what());
				std::this_thread::sleep_for(std::chrono::milliseconds(15000));
			}
		}

		return nullptr;
	}
}

class ThreadFactory {
public:
	static std::thread CreateThread(const std::string& name, std::function<void()> func) {
		return std::thread([name, func]() {
            pthread_setname_np(pthread_self(), name.c_str());
			ThreadStackManager::ConfigureThreadStack();
			func();
			});
	}

	static bool CreateDetachedThread(const std::string& name, std::function<void()> func) {
		try {
			std::thread t = CreateThread(name, func);
			t.detach();
			return true;
		}
		catch (const std::exception& e) {
            RAYCAST_DEBUG( "Failed to create detached thread %s: %s" , name.c_str(), e.what());
			return false;
		}
	}
};

class SceneManager {
private:
	std::vector<std::thread> threads_;
	std::atomic<bool> initialized_{ false };

public:
	bool Initialize() {
		try {
			ThreadStackManager::ConfigureThreadStack();

			auto height_field_scene = std::make_shared<Physics::VisibleScene<uint64_t, Int64Hash>>(Physics::int64Extractor);
			auto dynamic_rigid_scene = std::make_shared<Physics::VisibleScene<PrunerPayload, PrunerPayloadHash>>(Physics::prunerPayloadExtractor);
			auto dynamic_load_scene = std::make_shared<Physics::VisibleScene<PrunerPayload, PrunerPayloadHash>>(Physics::prunerPayloadExtractor);

			if (!height_field_scene || !dynamic_rigid_scene || !dynamic_load_scene) {
				return false;
			}

			{
				std::lock_guard<std::mutex> lock1(g_dynamic_load_scene_mutex);
				std::lock_guard<std::mutex> lock2(g_dynamic_rigid_scene_mutex);
				std::lock_guard<std::mutex> lock3(g_height_field_scene_mutex);
				DynamicLoadScene = dynamic_load_scene;
				DynamicRigidScene = dynamic_rigid_scene;
				HeightFieldScene = height_field_scene;
			}

			g_shutdown_requested.store(false, std::memory_order_release);


			bool success = true;

            CORE_LOG( "[Debug] Initializing Height Field Scene..." );

			success &= ThreadFactory::CreateDetachedThread("HeightFieldUpdater", []() {
				VisibleCheck::UpdateDynamicHeightField(nullptr);});

            CORE_LOG( "[Debug] Initializing Dynamic Rigid Scene..." );

			success &= ThreadFactory::CreateDetachedThread("DynamicRigidUpdater", []() {
				VisibleCheck::UpdateDynamicRigid(nullptr);});

            CORE_LOG( "[Debug] Initializing Dynamic Load Scene..." );

			success &= ThreadFactory::CreateDetachedThread("SceneRangeUpdater", []() {
				VisibleCheck::UpdateSceneByRange(nullptr);});

			if (!success) {
				Shutdown();
				return false;
			}

            CORE_LOG( "[Debug] Starting scene update threads..." );

			initialized_.store(true, std::memory_order_release);
			return true;

            CORE_LOG( "[Debug] All scene update threads started successfully" );

		}
		catch (const std::exception& e) {
            CORE_DEBUG( "Scene manager initialization exception: %s" , e.what());
			Shutdown();
			return false;
		}
	}

	void Shutdown() {

		g_shutdown_requested.store(true, std::memory_order_release);

		std::this_thread::sleep_for(std::chrono::milliseconds(200));

		{
			std::lock_guard<std::mutex> lock1(g_dynamic_load_scene_mutex);
			std::lock_guard<std::mutex> lock2(g_dynamic_rigid_scene_mutex);
			std::lock_guard<std::mutex> lock3(g_height_field_scene_mutex);
			DynamicLoadScene = nullptr;
			DynamicRigidScene = nullptr;
			HeightFieldScene = nullptr;
		}

		initialized_.store(false, std::memory_order_release);
        CORE_WARN( "Scene loading has stopped" );
	}

	bool IsInitialized() const {
		return initialized_.load(std::memory_order_acquire);
	}
};

namespace LineTrace {
    static physx::PxVec3 ToPx(const FVector& v) {
        return physx::PxVec3(v.X, v.Y, v.Z);
    }

    inline bool ValidateTraceInput(const FVector& location, const FVector& traceEnd) {
        return std::isfinite(location.X) && std::isfinite(location.Y) && std::isfinite(location.Z) && std::isfinite(traceEnd.X) && std::isfinite(traceEnd.Y) && std::isfinite(traceEnd.Z);
    }

    // ==================== 帧级场景指针缓存 ====================
    // 优化核心：在一帧的绘制循环开始时缓存场景指针（只锁一次），
    // 整帧内所有的 LineTrace 调用都使用缓存指针，避免每次调用锁3个mutex。
    // 典型场景：5个敌人 × 每人最多15次射线检测 = 75次调用 → 从75×3=225次mutex锁降为1×3=3次。
    struct FrameCache {
        std::shared_ptr<Physics::VisibleScene<PrunerPayload, PrunerPayloadHash>> dynamic_load_scene;
        std::shared_ptr<Physics::VisibleScene<uint64_t, Int64Hash>> height_field_scene;
        std::shared_ptr<Physics::VisibleScene<PrunerPayload, PrunerPayloadHash>> dynamic_rigid_scene;
        bool valid = false;
    };

    static FrameCache g_frame_cache;

    // 帧开始时调用：缓存三个场景的 shared_ptr（只锁一次）
    inline void BeginFrame() {
        {
            std::lock_guard<std::mutex> lock1(g_dynamic_load_scene_mutex);
            std::lock_guard<std::mutex> lock2(g_dynamic_rigid_scene_mutex);
            std::lock_guard<std::mutex> lock3(g_height_field_scene_mutex);
            g_frame_cache.dynamic_load_scene = DynamicLoadScene;
            g_frame_cache.dynamic_rigid_scene = DynamicRigidScene;
            g_frame_cache.height_field_scene = HeightFieldScene;
        }
        g_frame_cache.valid = (g_frame_cache.dynamic_load_scene && g_frame_cache.height_field_scene && g_frame_cache.dynamic_rigid_scene);
    }

    // 帧结束时调用：释放缓存（防止长期持有 shared_ptr 阻碍更新线程）
    inline void EndFrame() {
        g_frame_cache.dynamic_load_scene.reset();
        g_frame_cache.dynamic_rigid_scene.reset();
        g_frame_cache.height_field_scene.reset();
        g_frame_cache.valid = false;
    }

    // 高性能版LineTrace：使用帧缓存 + rtcOccluded1 遮挡查询（无需计算交点细节）
    // 性能提升：1) 无mutex锁开销  2) rtcOccluded1比rtcIntersect1快（可提前退出）
    // 返回 true = 未被遮挡（可见），false = 被遮挡
    inline bool LineTraceSingleCached(FVector Location, FVector TraceEnd) {
        if (!g_frame_cache.valid) {
            return true; // 无可用场景，假设可见
        }
        if (!ValidateTraceInput(Location, TraceEnd)) {
            return true;
        }

        physx::PxVec3 origin = ToPx(Location);
        physx::PxVec3 target = ToPx(TraceEnd);

        // 使用 OcclusionTest（rtcOccluded1）—— 比 Raycast（rtcIntersect1）更快
        // 任一场景命中即被遮挡，立即返回 false
        if (g_frame_cache.dynamic_load_scene->OcclusionTest(origin, target)) {
            return false;
        }
        if (g_frame_cache.height_field_scene->OcclusionTest(origin, target)) {
            return false;
        }
        if (g_frame_cache.dynamic_rigid_scene->OcclusionTest(origin, target)) {
            return false;
        }

        return true; // 全部场景均未命中 → 未被遮挡
    }

    // 原始版 LineTrace（保留兼容性，非热路径可继续使用）
    inline bool LineTraceSingle(FVector Location, FVector TraceEnd) {
        if (!ValidateTraceInput(Location, TraceEnd)) {
            return true;
        }

        std::shared_ptr<Physics::VisibleScene<PrunerPayload, PrunerPayloadHash>> dynamic_load_scene;
        std::shared_ptr<Physics::VisibleScene<uint64_t, Int64Hash>> height_field_scene;
        std::shared_ptr<Physics::VisibleScene<PrunerPayload, PrunerPayloadHash>> dynamic_rigid_scene;

        {
            std::lock_guard<std::mutex> lock1(g_dynamic_load_scene_mutex);
            std::lock_guard<std::mutex> lock2(g_dynamic_rigid_scene_mutex);
            std::lock_guard<std::mutex> lock3(g_height_field_scene_mutex);
            dynamic_load_scene = DynamicLoadScene;
            dynamic_rigid_scene = DynamicRigidScene;
            height_field_scene = HeightFieldScene;
        }

        if (!dynamic_load_scene || !height_field_scene || !dynamic_rigid_scene) {
            return true;
        }

        physx::PxVec3 origin = ToPx(Location);
        physx::PxVec3 target = ToPx(TraceEnd);

        auto dynamicRayHit = dynamic_load_scene->Raycast(origin, target);
        if (dynamicRayHit.hit.geomID != RTC_INVALID_GEOMETRY_ID) {
            return false;
        }

        auto heightFieldRayHit = height_field_scene->Raycast(origin, target);
        if (heightFieldRayHit.hit.geomID != RTC_INVALID_GEOMETRY_ID) {
            return false;
        }

        auto globalSceneRayHit = dynamic_rigid_scene->Raycast(origin, target);
        if (globalSceneRayHit.hit.geomID != RTC_INVALID_GEOMETRY_ID) {
            return false;
        }

        return true;
    }

    inline TriangleMeshData* getNextHint(FRotator& playerRotation, FVector& playerLocation, float fov) {
        if (!ValidateTraceInput(playerLocation, playerLocation)) {
            return nullptr;
        }

        std::shared_ptr<Physics::VisibleScene<PrunerPayload, PrunerPayloadHash>> dynamic_load_scene;
        std::shared_ptr<Physics::VisibleScene<uint64_t, Int64Hash>> height_field_scene;
        std::shared_ptr<Physics::VisibleScene<PrunerPayload, PrunerPayloadHash>> dynamic_rigid_scene;

        {
            std::lock_guard<std::mutex> lock1(g_dynamic_load_scene_mutex);
            std::lock_guard<std::mutex> lock2(g_dynamic_rigid_scene_mutex);
            std::lock_guard<std::mutex> lock3(g_height_field_scene_mutex);
            dynamic_load_scene = DynamicLoadScene;
            dynamic_rigid_scene = DynamicRigidScene;
            height_field_scene = HeightFieldScene;
        }

        if (!dynamic_load_scene || !height_field_scene || !dynamic_rigid_scene) {
            return nullptr;
        }

        FVector forwardVector;
        forwardVector.X = cos(playerRotation.Yaw * M_PI / 180.0f) * cos(playerRotation.Pitch * M_PI / 180.0f);
        forwardVector.Y = sin(playerRotation.Yaw * M_PI / 180.0f) * cos(playerRotation.Pitch * M_PI / 180.0f);
        forwardVector.Z = sin(playerRotation.Pitch * M_PI / 180.0f);

        physx::PxVec3 origin = ToPx(playerLocation);
        physx::PxVec3 target = ToPx(playerLocation + forwardVector * 100000.0f);

        // 射线检测
        auto staticRayHit = dynamic_load_scene->Raycast(origin, target);
        auto heightFieldRayHit = height_field_scene->Raycast(origin, target);
        auto dynamicRayHit = dynamic_rigid_scene->Raycast(origin, target);

        float minDist = FLT_MAX;
        TriangleMeshData* result = nullptr;

        // 命中结果
        auto updateResult = [&](const RTCRayHit& hit, auto& scene) {
            if (hit.hit.geomID != RTC_INVALID_GEOMETRY_ID && hit.ray.tfar < minDist) {
                minDist = hit.ray.tfar;
                result = scene->GetGeometryData(hit.hit.geomID);
            }
        };

        updateResult(staticRayHit, dynamic_load_scene);
        updateResult(heightFieldRayHit, height_field_scene);
        updateResult(dynamicRayHit, dynamic_rigid_scene);

        return result;
    }
}

static SceneManager g_scene_manager;

inline void StartLoadMapModel() {
	if (!g_scene_manager.Initialize()) {
        CORE_WARN( "Failed to load startup scene" );
	}
}

inline void StopLoadMapModel() {
    CORE_WARN( "Starting load map model..." );
	g_scene_manager.Shutdown();
}

inline void ExportToOBJ(const TriangleMeshData& mesh, const std::string& filename) {
    size_t meshHash = mesh.HashValue;
    if (meshHash == 0) {
        meshHash = mesh.ComputeHash();
    }

    if (exportedHashes.find(meshHash) != exportedHashes.end()) {
        CORE_LOG("Mesh already exported (hash: %zu), skipping.", meshHash);
        return;
    }

    static std::vector<TriangleMeshData> exportedMeshes;
    for (const auto& exportedMesh : exportedMeshes) {
        if (mesh.IsEqual(exportedMesh)) {
            CORE_LOG("Mesh already exported (exact match), skipping.");
            return;
        }
    }

    std::string dir = filename.substr(0, filename.find_last_of("/"));
    if (!dir.empty()) {
        mkdir(dir.c_str(), 0777);
    }

    std::ofstream file(filename);
    if (!file.is_open()) {
        CORE_LOG("Unable to open file %s: %s", filename.c_str(), strerror(errno));
        return;
    }

    if (mesh.Vertices.empty() || mesh.Indices.empty()) {
        CORE_LOG("Mesh is empty, skip export.");
        return;
    }

    physx::PxVec3 min = mesh.Vertices[0];
    physx::PxVec3 max = mesh.Vertices[0];
    for (const auto& v : mesh.Vertices) {
        min.x = std::min(min.x, v.x);
        min.y = std::min(min.y, v.y);
        min.z = std::min(min.z, v.z);
        max.x = std::max(max.x, v.x);
        max.y = std::max(max.y, v.y);
        max.z = std::max(max.z, v.z);
    }

    file << "# Exported from PUBGM PhysX Model Extractor\n";
    file << "# Vertices: " << mesh.Vertices.size() << "\n";
    file << "# Faces: " << mesh.Indices.size() / 3 << "\n";
    file << "# Bounding box: " << min.x << " " << min.y << " " << min.z << " to " << max.x << " " << max.y << " " << max.z << "\n\n";

    for (const auto& vertex : mesh.Vertices) {
        file << "v " << vertex.x << " " << vertex.z << " " << vertex.y << "\n";
    }

    for (size_t i = 0; i < mesh.Indices.size(); i += 3) {
        if (i + 2 < mesh.Indices.size()) {
            uint32_t idx1 = mesh.Indices[i] + 1;
            uint32_t idx2 = mesh.Indices[i + 1] + 1;
            uint32_t idx3 = mesh.Indices[i + 2] + 1;

            if (idx1 > mesh.Vertices.size() || idx2 > mesh.Vertices.size() || idx3 > mesh.Vertices.size()) {
                CORE_LOG("Warning: Invalid index found at position %zu: %u, %u, %u (max: %zu)\n",
                       i, idx1, idx2, idx3, mesh.Vertices.size());
                continue;
            }

            file << "f " << idx1 << " " << idx3 << " " << idx2 << "\n";
        }
    }

    file.close();
    exportedHashes.insert(meshHash);
}

inline void ExportAllModels() {
    std::vector<PrunerPayload> staticObjects = PhysX::CollectAllStaticRigidActorShapes();
    auto staticMeshes = PhysX::GetMeshDataOptimized(staticObjects,[](const ShapeDataT& shape_data) {
        PxGeometryType type = shape_data.mType;
        uint8_t Flags = shape_data.m_shape_flags;
        return !((type == PxGeometryType::eTRIANGLEMESH || type == PxGeometryType::eBOX || type == PxGeometryType::eCONVEXMESH || type == PxGeometryType::eCAPSULE && type == PxGeometryType::eSPHERE) && ((Flags & 0xF) == 0x2 || (Flags & 0xF) == 0x3 || (Flags & 0xF) == 0xB));},false);

    std::vector<PrunerPayload> heightFieldObjects = PhysX::CollectAllHeightRigidActorShapes();
    auto heightFieldMeshes = PhysX::GetMeshDataOptimized(heightFieldObjects,[](const ShapeDataT& shape_data) {
        PxGeometryType type = shape_data.mType;
        return type != PxGeometryType::eHEIGHTFIELD;});

    std::vector<PrunerPayload> dynamicObjects = PhysX::CollectAllDynamicRigidActorShapes();
    auto dynamicMeshes = PhysX::GetMeshDataOptimized(dynamicObjects,[](const ShapeDataT& shape_data) {PxGeometryType type = shape_data.mType;
        uint8_t Flags = shape_data.m_shape_flags;
        return !((type == PxGeometryType::eTRIANGLEMESH || type == PxGeometryType::eBOX || type == PxGeometryType::eCONVEXMESH || type == PxGeometryType::eCAPSULE && type == PxGeometryType::eSPHERE) && ((Flags & 0xF) == 0x2 || (Flags & 0xF) == 0x3 || (Flags & 0xF) == 0xB));
        },true
    );

    int counter = 0;
    for (const auto& mesh : staticMeshes) {
        char filename[256];
        snprintf(filename, sizeof(filename), "/sdcard/Models/static_mesh_%d.obj", counter++);
        ExportToOBJ(mesh, filename);
    }

    for (const auto& mesh : heightFieldMeshes) {
        char filename[256];
        snprintf(filename, sizeof(filename), "/sdcard/Models/heightfield_mesh_%d.obj", counter++);
        ExportToOBJ(mesh, filename);
    }

    for (const auto& mesh : dynamicMeshes) {
        char filename[256];
        snprintf(filename, sizeof(filename), "/sdcard/Models/dynamic_mesh_%d.obj", counter++);
        ExportToOBJ(mesh, filename);
    }

    printf("Exported %d models in total\n", counter);
}

#endif