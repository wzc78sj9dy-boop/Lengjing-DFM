#include "game/native/GeometryRuntime.h"
#include "game/native/GeometrySceneBuildPolicy.h"
#include "game/native/GeometryShapeFilterPolicy.h"

#include "embree4/rtcore.h"
#include "embree4/rtcore_ray.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if defined(__ANDROID__)
#include <sys/resource.h>
#endif

namespace lengjing::game::native {
namespace {

constexpr std::uintptr_t kMinimumRemoteAddress = 0x10000ULL;
constexpr std::uintptr_t kMaximumRemoteAddress = 0x0000FFFFFFFFFFFFULL;
constexpr std::uintptr_t kSceneArrayOffset = 8ULL;
constexpr std::uintptr_t kActorArrayOffset = 9704ULL;
constexpr std::uintptr_t kSceneQueryManagerOffset = 0x2430ULL;
constexpr std::uintptr_t kPruningPoolOffset = 0x1A0ULL;
constexpr std::size_t kPrunerStride = 0x30;
constexpr std::size_t kStaticPrunerIndex = 0;
constexpr std::size_t kDynamicPrunerIndex = 1;
constexpr std::size_t kMaximumScenes = 64;
constexpr std::size_t kMaximumActorsPerScene = 262144;
constexpr std::size_t kMaximumPrunerObjects = 262144;
constexpr std::size_t kMaximumShapesPerActor = 4096;
constexpr std::size_t kMaximumReadChunk = 1024 * 1024;
constexpr std::size_t kMissingMeshRetentionRounds = 3;
constexpr std::uint16_t kDynamicActorType = 6;
constexpr std::uint16_t kStaticActorType = 7;
constexpr std::uint16_t kTriangleMeshBvh33Type = 3;
constexpr std::uint16_t kTriangleMeshBvh34Type = 4;
constexpr std::uint16_t kHeightFieldType = 1;
constexpr std::uint16_t kConvexMeshType = 2;
constexpr std::int32_t kSphereGeometryType = 0;
constexpr std::int32_t kPlaneGeometryType = 1;
constexpr std::int32_t kCapsuleGeometryType = 2;
constexpr std::int32_t kBoxGeometryType = 3;
constexpr std::int32_t kConvexMeshGeometryType = 4;
constexpr std::int32_t kTriangleMeshGeometryType = 5;
constexpr std::int32_t kHeightFieldGeometryType = 6;
constexpr std::int32_t kGeometryTypeCount = 7;
constexpr std::uint8_t kMeshHas16BitIndices = 0x02U;
constexpr std::uint8_t kHeightFieldTessellationFlag = 0x80U;
constexpr std::uint8_t kHeightFieldMaterialMask = 0x7FU;
constexpr std::uint8_t kHeightFieldHoleMaterial = 0x7FU;
constexpr float kMaximumGeometryExtent = 1.0e7f;
constexpr float kPi = 3.14159265358979323846f;
constexpr int kRoundSegments = 12;
constexpr int kSphereRings = 8;
constexpr int kCapsuleHemisphereRings = 6;

struct RemoteArray {
    std::uintptr_t data = 0;
    std::int32_t count = 0;
    std::int32_t capacity = 0;
};
static_assert(sizeof(RemoteArray) == 16, "Remote array layout mismatch");

struct RemoteVec3 {
    float x;
    float y;
    float z;
};
static_assert(sizeof(RemoteVec3) == 12, "Remote vector layout mismatch");

struct RemoteQuat {
    float x;
    float y;
    float z;
    float w;
};
static_assert(sizeof(RemoteQuat) == 16, "Remote quaternion layout mismatch");

struct RemoteTransform {
    RemoteQuat q;
    RemoteVec3 p;
};
static_assert(sizeof(RemoteTransform) == 28, "Remote transform layout mismatch");

struct RemoteMeshScale {
    RemoteVec3 scale;
    RemoteQuat rotation;
};
static_assert(sizeof(RemoteMeshScale) == 28, "Remote mesh scale layout mismatch");

struct RemoteRigidCore {
    alignas(16) RemoteTransform bodyToWorld;
    std::uint8_t flags;
    std::uint8_t bodyToActorIdentity;
    std::uint16_t solverIterations;
};
static_assert(sizeof(RemoteRigidCore) == 32, "Remote rigid core layout mismatch");

struct RemoteBodyCore {
    std::array<std::uint8_t, 0x10> padding;
    alignas(16) RemoteRigidCore core;
    alignas(16) RemoteTransform bodyToActor;
};
static_assert(sizeof(RemoteBodyCore) == 80, "Remote body core layout mismatch");
static_assert(offsetof(RemoteBodyCore, core) == 16,
              "Remote rigid core offset mismatch");
static_assert(offsetof(RemoteBodyCore, bodyToActor) == 48,
              "Remote body-to-actor offset mismatch");

struct RemoteBody {
    std::array<std::uint8_t, 0x60> padding;
    std::uint64_t scene;
    std::uint64_t controlState;
    std::uint64_t stream;
    alignas(16) RemoteBodyCore rigid;
};
static_assert(sizeof(RemoteBody) == 208, "Remote body layout mismatch");
static_assert(offsetof(RemoteBody, rigid) == 128,
              "Remote body core offset mismatch");

struct RemoteFilterData {
    std::uint32_t word0;
    std::uint32_t word1;
    std::uint32_t word2;
    std::uint32_t word3;
};
static_assert(sizeof(RemoteFilterData) == 16, "Remote filter layout mismatch");

struct alignas(8) RemoteGeometryUnion {
    std::array<std::uint8_t, 80> data;
};
static_assert(sizeof(RemoteGeometryUnion) == 80,
              "Remote geometry union layout mismatch");

struct RemoteShapeCoreData {
    alignas(16) RemoteTransform transform;
    float contactOffset;
    std::uint8_t shapeFlags;
    std::uint8_t ownsMaterialIndexMemory;
    std::uint16_t materialIndex;
    RemoteGeometryUnion geometry;
};
static_assert(sizeof(RemoteShapeCoreData) == 128,
              "Remote shape core data layout mismatch");
static_assert(offsetof(RemoteShapeCoreData, geometry) == 40,
              "Remote geometry offset mismatch");

struct RemoteShapeCore {
    RemoteFilterData queryFilter;
    RemoteFilterData simulationFilter;
    alignas(16) RemoteShapeCoreData core;
    float restOffset;
};
static_assert(sizeof(RemoteShapeCore) == 176,
              "Remote shape core layout mismatch");
static_assert(offsetof(RemoteShapeCore, core) == 32,
              "Remote shape core data offset mismatch");

struct RemoteShape {
    std::array<std::uint8_t, 0x30> padding;
    std::uint64_t scene;
    std::uint32_t controlState;
    std::uint64_t stream;
    alignas(16) RemoteShapeCore shapeCore;
};
static_assert(sizeof(RemoteShape) == 256, "Remote shape layout mismatch");
static_assert(offsetof(RemoteShape, shapeCore) == 80,
              "Remote shape core offset mismatch");

struct RemotePruningPool {
    std::uint32_t objectCount;
    std::uint32_t maximumObjectCount;
    std::uintptr_t worldBounds;
    std::uintptr_t objects;
};
static_assert(sizeof(RemotePruningPool) == 24,
              "Remote pruning pool layout mismatch");
static_assert(offsetof(RemotePruningPool, objects) == 16,
              "Remote pruning object pointer offset mismatch");

struct RemotePrunerPayload {
    std::uintptr_t shape;
    std::uintptr_t actor;
};
static_assert(sizeof(RemotePrunerPayload) == 0x10,
              "Remote pruner payload layout mismatch");

struct GeometryBodyData {
    RemoteBodyCore rigid{};
};

struct GeometryShapeData {
    RemoteShapeCore shapeCore{};
};

struct RemoteCenterExtents {
    RemoteVec3 center;
    RemoteVec3 extents;
};
static_assert(sizeof(RemoteCenterExtents) == 24,
              "Remote bounds layout mismatch");

struct RemoteTriangleMesh {
    std::array<std::uint8_t, 0x8> padding;
    std::uint16_t concreteType;
    std::uint16_t baseFlags;
    std::uint64_t refCountVtable;
    std::int32_t refCount;
    std::uint32_t vertexCount;
    std::uint32_t triangleCount;
    RemoteVec3* vertices;
    void* triangles;
    RemoteCenterExtents bounds;
    std::uint8_t* extraTriangleData;
    float geometryEpsilon;
    std::uint8_t flags;
};
static_assert(sizeof(RemoteTriangleMesh) == 96,
              "Remote triangle mesh layout mismatch");
static_assert(offsetof(RemoteTriangleMesh, vertices) == 40,
              "Remote vertex pointer offset mismatch");
static_assert(offsetof(RemoteTriangleMesh, triangles) == 48,
              "Remote index pointer offset mismatch");
static_assert(offsetof(RemoteTriangleMesh, flags) == 92,
              "Remote mesh flags offset mismatch");

struct RemoteTriangleMeshGeometry {
    std::int32_t type;
    RemoteMeshScale scale;
    std::uint8_t meshFlags;
    std::array<std::uint8_t, 3> padding;
    RemoteTriangleMesh* triangleMesh;
};
static_assert(sizeof(RemoteTriangleMeshGeometry) == 48,
              "Remote triangle geometry layout mismatch");
static_assert(offsetof(RemoteTriangleMeshGeometry, triangleMesh) == 40,
              "Remote triangle mesh pointer offset mismatch");

struct RemoteSphereGeometry {
    std::int32_t type;
    float radius;
};
static_assert(sizeof(RemoteSphereGeometry) == 8,
              "Remote sphere geometry layout mismatch");

struct RemoteCapsuleGeometry {
    std::int32_t type;
    float radius;
    float halfHeight;
};
static_assert(sizeof(RemoteCapsuleGeometry) == 12,
              "Remote capsule geometry layout mismatch");

struct RemoteBoxGeometry {
    std::int32_t type;
    RemoteVec3 halfExtents;
};
static_assert(sizeof(RemoteBoxGeometry) == 16,
              "Remote box geometry layout mismatch");

struct RemotePlane {
    RemoteVec3 normal;
    float distance;
};
static_assert(sizeof(RemotePlane) == 16,
              "Remote plane layout mismatch");

struct RemoteHullPolygon {
    RemotePlane plane;
    std::uint16_t vertexReference;
    std::uint8_t vertexCount;
    std::uint8_t minimumIndex;
};
static_assert(sizeof(RemoteHullPolygon) == 20,
              "Remote hull polygon layout mismatch");

struct RemoteConvexHullData {
    RemoteCenterExtents bounds;
    RemoteVec3 centerOfMass;
    std::uint16_t edgeCountAndFlags;
    std::uint8_t vertexCount;
    std::uint8_t polygonCount;
    RemoteHullPolygon* polygons;
};
static_assert(sizeof(RemoteConvexHullData) == 48,
              "Remote convex hull layout mismatch");
static_assert(offsetof(RemoteConvexHullData, polygons) == 40,
              "Remote convex polygon pointer offset mismatch");

struct RemoteConvexMesh {
    std::array<std::uint8_t, 0x8> padding;
    std::uint16_t concreteType;
    std::uint16_t baseFlags;
    std::uint64_t refCountVtable;
    std::int32_t refCount;
    RemoteConvexHullData hull;
    std::uint32_t auxiliaryCount;
};
static_assert(sizeof(RemoteConvexMesh) == 88,
              "Remote convex mesh layout mismatch");
static_assert(offsetof(RemoteConvexMesh, hull) == 32,
              "Remote convex hull offset mismatch");

struct RemoteConvexMeshGeometry {
    std::int32_t type;
    RemoteMeshScale scale;
    RemoteConvexMesh* convexMesh;
    float maximumMargin;
    std::uint8_t meshFlags;
    std::array<std::uint8_t, 3> padding;
};
static_assert(sizeof(RemoteConvexMeshGeometry) == 48,
              "Remote convex geometry layout mismatch");
static_assert(offsetof(RemoteConvexMeshGeometry, convexMesh) == 32,
              "Remote convex mesh pointer offset mismatch");

struct RemoteHeightFieldSample {
    std::int16_t height;
    std::uint8_t material0;
    std::uint8_t material1;
};
static_assert(sizeof(RemoteHeightFieldSample) == 4,
              "Remote height sample layout mismatch");

struct RemoteHeightFieldData {
    RemoteCenterExtents bounds;
    std::uint32_t rows;
    std::uint32_t columns;
    float rowLimit;
    float columnLimit;
    float columnCount;
    RemoteHeightFieldSample* samples;
    float thickness;
    float convexEdgeThreshold;
    std::uint16_t flags;
    std::uint8_t format;
};
static_assert(sizeof(RemoteHeightFieldData) == 72,
              "Remote height data layout mismatch");
static_assert(offsetof(RemoteHeightFieldData, samples) == 48,
              "Remote height sample pointer offset mismatch");

struct RemoteHeightField {
    std::array<std::uint8_t, 0x8> padding;
    std::uint16_t concreteType;
    std::uint16_t baseFlags;
    std::uint64_t refCountVtable;
    std::int32_t refCount;
    RemoteHeightFieldData data;
    std::uint32_t sampleStride;
    std::uint32_t sampleCount;
    float minimumHeight;
    float maximumHeight;
    std::int32_t modifyCount;
    void* meshFactory;
};
static_assert(sizeof(RemoteHeightField) == 136,
              "Remote height field layout mismatch");
static_assert(offsetof(RemoteHeightField, data) == 32,
              "Remote height data offset mismatch");

struct RemoteHeightFieldGeometry {
    std::int32_t type;
    RemoteHeightField* heightField;
    float heightScale;
    float rowScale;
    float columnScale;
    std::int8_t flags;
    std::array<std::uint8_t, 3> padding;
};
static_assert(sizeof(RemoteHeightFieldGeometry) == 32,
              "Remote height geometry layout mismatch");
static_assert(offsetof(RemoteHeightFieldGeometry, heightField) == 8,
              "Remote height field pointer offset mismatch");
static_assert(sizeof(GeometryPoint) == sizeof(float) * 3,
              "Published vertex layout mismatch");

struct ActorShape {
    std::uintptr_t actor = 0;
    std::uintptr_t shape = 0;

    bool operator==(const ActorShape& other) const noexcept {
        return actor == other.actor && shape == other.shape;
    }
};

struct ActorShapeHash {
    std::size_t operator()(const ActorShape& value) const noexcept {
        const auto first = static_cast<std::size_t>(value.actor);
        const auto second = static_cast<std::size_t>(value.shape);
        return first ^ (second + 0x9e3779b9U + (first << 6U) + (first >> 2U));
    }
};

bool ActorShapeLess(const ActorShape& left,
                    const ActorShape& right) noexcept {
    if (left.actor != right.actor) {
        return left.actor < right.actor;
    }
    if (left.shape != right.shape) {
        return left.shape < right.shape;
    }
    return false;
}

bool PrunerPayloadLess(const RemotePrunerPayload& left,
                       const RemotePrunerPayload& right) noexcept {
    if (left.actor != right.actor) {
        return left.actor < right.actor;
    }
    return left.shape < right.shape;
}

bool RemoteArrayEqual(const RemoteArray& left,
                      const RemoteArray& right) noexcept {
    return left.data == right.data && left.count == right.count &&
           left.capacity == right.capacity;
}

bool PruningPoolEqual(const RemotePruningPool& left,
                      const RemotePruningPool& right) noexcept {
    return left.objectCount == right.objectCount &&
           left.maximumObjectCount == right.maximumObjectCount &&
           left.worldBounds == right.worldBounds &&
           left.objects == right.objects;
}

bool PrunerPayloadsEqual(
    const std::vector<RemotePrunerPayload>& left,
    const std::vector<RemotePrunerPayload>& right) noexcept {
    return left.size() == right.size() &&
           std::equal(
               left.begin(), left.end(), right.begin(),
               [](const RemotePrunerPayload& first,
                  const RemotePrunerPayload& second) {
                   return first.actor == second.actor &&
                          first.shape == second.shape;
               });
}

struct CollectionReport {
    bool sourceAvailable = false;
    bool complete = false;
    bool budgetLimited = false;
    bool partial = false;
};

using ActorShapeSet = std::unordered_set<ActorShape, ActorShapeHash>;
using ActorShapeMissMap =
    std::unordered_map<ActorShape, std::size_t, ActorShapeHash>;

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

struct Transform {
    Quat q{};
    Vec3 p{};
};

bool IsFinite(float value) noexcept {
    return std::isfinite(value);
}

bool IsFinite(const Vec3& value) noexcept {
    return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z);
}

bool IsFinite(const GeometryPoint& value) noexcept {
    return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z);
}

bool IsReasonable(const Vec3& value) noexcept {
    constexpr float kMaximumCoordinate = 1.0e9f;
    return IsFinite(value) && std::abs(value.x) <= kMaximumCoordinate &&
           std::abs(value.y) <= kMaximumCoordinate &&
           std::abs(value.z) <= kMaximumCoordinate;
}

Vec3 Add(const Vec3& left, const Vec3& right) noexcept {
    return Vec3{left.x + right.x, left.y + right.y, left.z + right.z};
}

Vec3 Subtract(const Vec3& left, const Vec3& right) noexcept {
    return Vec3{left.x - right.x, left.y - right.y, left.z - right.z};
}

Vec3 Multiply(const Vec3& value, float scalar) noexcept {
    return Vec3{value.x * scalar, value.y * scalar, value.z * scalar};
}

Vec3 Multiply(const Vec3& left, const Vec3& right) noexcept {
    return Vec3{left.x * right.x, left.y * right.y, left.z * right.z};
}

float Dot(const Vec3& left, const Vec3& right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

Vec3 Cross(const Vec3& left, const Vec3& right) noexcept {
    return Vec3{left.y * right.z - left.z * right.y,
                left.z * right.x - left.x * right.z,
                left.x * right.y - left.y * right.x};
}

float LengthSquared(const Vec3& value) noexcept {
    return Dot(value, value);
}

bool Normalize(Quat& value) noexcept {
    const float lengthSquared =
        value.x * value.x + value.y * value.y + value.z * value.z +
        value.w * value.w;
    if (!IsFinite(lengthSquared) || lengthSquared < 0.25f ||
        lengthSquared > 4.0f) {
        return false;
    }
    const float inverseLength = 1.0f / std::sqrt(lengthSquared);
    value.x *= inverseLength;
    value.y *= inverseLength;
    value.z *= inverseLength;
    value.w *= inverseLength;
    return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z) &&
           IsFinite(value.w);
}

Quat Conjugate(const Quat& value) noexcept {
    return Quat{-value.x, -value.y, -value.z, value.w};
}

Quat Multiply(const Quat& left, const Quat& right) noexcept {
    return Quat{
        left.w * right.x + left.x * right.w + left.y * right.z -
            left.z * right.y,
        left.w * right.y - left.x * right.z + left.y * right.w +
            left.z * right.x,
        left.w * right.z + left.x * right.y - left.y * right.x +
            left.z * right.w,
        left.w * right.w - left.x * right.x - left.y * right.y -
            left.z * right.z};
}

Vec3 Rotate(const Quat& rotation, const Vec3& value) noexcept {
    const Vec3 vectorPart{rotation.x, rotation.y, rotation.z};
    const Vec3 twiceCross = Multiply(Cross(vectorPart, value), 2.0f);
    return Add(value,
               Add(Multiply(twiceCross, rotation.w),
                   Cross(vectorPart, twiceCross)));
}

Transform Compose(const Transform& parent, const Transform& child) noexcept {
    Transform result;
    result.q = Multiply(parent.q, child.q);
    Normalize(result.q);
    result.p = Add(Rotate(parent.q, child.p), parent.p);
    return result;
}

Transform Inverse(const Transform& value) noexcept {
    Transform result;
    result.q = Conjugate(value.q);
    result.p = Rotate(result.q, Multiply(value.p, -1.0f));
    return result;
}

Vec3 Apply(const Transform& transform, const Vec3& value) noexcept {
    return Add(Rotate(transform.q, value), transform.p);
}

bool Convert(const RemoteTransform& source, Transform& destination) noexcept {
    destination.q =
        Quat{source.q.x, source.q.y, source.q.z, source.q.w};
    destination.p = Vec3{source.p.x, source.p.y, source.p.z};
    return Normalize(destination.q) && IsReasonable(destination.p);
}

bool Convert(const RemoteMeshScale& source, Vec3& scale,
             Quat& rotation) noexcept {
    scale = Vec3{source.scale.x, source.scale.y, source.scale.z};
    rotation =
        Quat{source.rotation.x, source.rotation.y, source.rotation.z,
             source.rotation.w};
    constexpr float kMaximumScale = 10000.0f;
    if (!IsFinite(scale) || std::abs(scale.x) > kMaximumScale ||
        std::abs(scale.y) > kMaximumScale ||
        std::abs(scale.z) > kMaximumScale ||
        std::abs(scale.x) <= 1.0e-4f ||
        std::abs(scale.y) <= 1.0e-4f ||
        std::abs(scale.z) <= 1.0e-4f) {
        return false;
    }
    return Normalize(rotation);
}

Vec3 ApplyMeshScale(const Vec3& value, const Vec3& scale,
                    const Quat& rotation) noexcept {
    return Rotate(Conjugate(rotation),
                  Multiply(Rotate(rotation, value), scale));
}

bool IsRemoteRange(std::uintptr_t address, std::size_t size,
                   std::size_t alignment = 1) noexcept {
    if (size == 0 || address < kMinimumRemoteAddress ||
        address > kMaximumRemoteAddress || alignment == 0 ||
        (address % alignment) != 0) {
        return false;
    }
    return size - 1 <= kMaximumRemoteAddress - address;
}

bool CheckedMultiply(std::size_t left, std::size_t right,
                     std::size_t& result) noexcept {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

bool CheckedAdd(std::size_t left, std::size_t right,
                std::size_t& result) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

bool IsValidExtent(float value) noexcept {
    return IsFinite(value) && value > 1.0e-4f &&
           value <= kMaximumGeometryExtent;
}

bool BuildWorldTransform(const GeometryBodyData& body,
                         GeometryBodyType bodyType,
                         const GeometryShapeData& shape,
                         Transform& worldTransform) noexcept {
    Transform bodyWorld{};
    Transform shapeLocal{};
    if (!Convert(body.rigid.core.bodyToWorld, bodyWorld) ||
        !Convert(shape.shapeCore.core.transform, shapeLocal)) {
        return false;
    }

    Transform actorWorld = bodyWorld;
    if (bodyType == GeometryBodyType::Dynamic) {
        Transform bodyToActor{};
        if (!Convert(body.rigid.bodyToActor, bodyToActor)) {
            return false;
        }
        actorWorld = Compose(bodyWorld, Inverse(bodyToActor));
    }
    worldTransform = Compose(actorWorld, shapeLocal);
    return IsReasonable(worldTransform.p);
}

bool FinalizeMesh(const ActorShape& reference, GeometryBodyType bodyType,
                  const Transform& worldTransform,
                  const std::vector<Vec3>& localVertices,
                  const std::vector<std::uint32_t>& sourceIndices,
                  std::shared_ptr<const GeometryMesh>& output) {
    output.reset();
    if (localVertices.size() < 3 || sourceIndices.size() < 3 ||
        (sourceIndices.size() % 3) != 0) {
        return false;
    }

    auto mesh = std::make_shared<GeometryMesh>();
    mesh->bodyType = bodyType;
    mesh->actorAddress = reference.actor;
    mesh->shapeAddress = reference.shape;
    mesh->vertices.resize(localVertices.size());

    double centerX = 0.0;
    double centerY = 0.0;
    double centerZ = 0.0;
    for (std::size_t index = 0; index < localVertices.size(); ++index) {
        if (!IsReasonable(localVertices[index])) {
            return false;
        }
        const Vec3 world = Apply(worldTransform, localVertices[index]);
        if (!IsReasonable(world)) {
            return false;
        }
        mesh->vertices[index] = GeometryPoint{world.x, world.y, world.z};
        centerX += static_cast<double>(world.x);
        centerY += static_cast<double>(world.y);
        centerZ += static_cast<double>(world.z);
    }

    mesh->indices.reserve(sourceIndices.size());
    for (std::size_t index = 0; index < sourceIndices.size(); index += 3) {
        const std::uint32_t first = sourceIndices[index];
        const std::uint32_t second = sourceIndices[index + 1];
        const std::uint32_t third = sourceIndices[index + 2];
        if (first >= mesh->vertices.size() ||
            second >= mesh->vertices.size() ||
            third >= mesh->vertices.size()) {
            return false;
        }
        if (first == second || second == third || first == third) {
            continue;
        }

        const auto& a = mesh->vertices[first];
        const auto& b = mesh->vertices[second];
        const auto& c = mesh->vertices[third];
        const Vec3 edgeA{b.x - a.x, b.y - a.y, b.z - a.z};
        const Vec3 edgeB{c.x - a.x, c.y - a.y, c.z - a.z};
        const float areaSquared = LengthSquared(Cross(edgeA, edgeB));
        if (!IsFinite(areaSquared) || areaSquared <= 1.0e-10f) {
            continue;
        }
        mesh->indices.push_back(first);
        mesh->indices.push_back(second);
        mesh->indices.push_back(third);
    }
    if (mesh->indices.empty()) {
        return false;
    }

    const double inverseCount =
        1.0 / static_cast<double>(mesh->vertices.size());
    mesh->center = GeometryPoint{
        static_cast<float>(centerX * inverseCount),
        static_cast<float>(centerY * inverseCount),
        static_cast<float>(centerZ * inverseCount)};
    if (!IsFinite(mesh->center)) {
        return false;
    }

    float maximumDistanceSquared = 0.0f;
    for (const auto& vertex : mesh->vertices) {
        const Vec3 delta{vertex.x - mesh->center.x,
                         vertex.y - mesh->center.y,
                         vertex.z - mesh->center.z};
        maximumDistanceSquared =
            std::max(maximumDistanceSquared, LengthSquared(delta));
    }
    if (!IsFinite(maximumDistanceSquared)) {
        return false;
    }
    mesh->boundsRadius = std::sqrt(maximumDistanceSquared);
    output = std::move(mesh);
    return true;
}

bool IsSameGeometryMeshContent(const GeometryMesh& left,
                               const GeometryMesh& right) noexcept {
    if (left.bodyType != right.bodyType ||
        left.actorAddress != right.actorAddress ||
        left.shapeAddress != right.shapeAddress ||
        left.center.x != right.center.x ||
        left.center.y != right.center.y ||
        left.center.z != right.center.z ||
        left.boundsRadius != right.boundsRadius ||
        left.vertices.size() != right.vertices.size() ||
        left.indices != right.indices) {
        return false;
    }
    return std::equal(
        left.vertices.begin(), left.vertices.end(), right.vertices.begin(),
        [](const GeometryPoint& first, const GeometryPoint& second) {
            return first.x == second.x && first.y == second.y &&
                first.z == second.z;
        });
}

GeometryRuntimeConfig Sanitize(GeometryRuntimeConfig config) {
    config.dynamicRefresh =
        std::clamp(config.dynamicRefresh, std::chrono::milliseconds(100),
                   std::chrono::milliseconds(5000));
    config.staticRefresh =
        std::clamp(config.staticRefresh, config.dynamicRefresh,
                   std::chrono::milliseconds(60000));
    config.lastGoodTtl =
        std::clamp(config.lastGoodTtl, config.dynamicRefresh,
                   std::chrono::milliseconds(60000));
    config.maxConsecutiveFailures =
        std::clamp<std::size_t>(config.maxConsecutiveFailures, 1, 1000);
    config.maxActors =
        std::clamp<std::size_t>(config.maxActors, 1, 262144);
    config.maxShapes =
        std::clamp<std::size_t>(config.maxShapes, 1, 262144);
    config.maxMeshes =
        std::clamp<std::size_t>(config.maxMeshes, 1, 24000);
    config.maxVerticesPerMesh =
        std::clamp<std::size_t>(config.maxVerticesPerMesh, 3, 2000000);
    config.maxTrianglesPerMesh =
        std::clamp<std::size_t>(config.maxTrianglesPerMesh, 1, 2000000);
    config.maxTotalVertices =
        std::clamp<std::size_t>(config.maxTotalVertices, 3, 8000000);
    config.maxTotalTriangles =
        std::clamp<std::size_t>(config.maxTotalTriangles, 1, 8000000);
    return config;
}

class DeviceOwner final {
public:
    DeviceOwner() : device_(rtcNewDevice("threads=1")) {}

    ~DeviceOwner() {
        if (device_ != nullptr) {
            rtcReleaseDevice(device_);
        }
    }

    DeviceOwner(const DeviceOwner&) = delete;
    DeviceOwner& operator=(const DeviceOwner&) = delete;

    RTCDevice Get() const noexcept {
        return device_;
    }

private:
    RTCDevice device_ = nullptr;
};

class SceneOwner final {
public:
    SceneOwner(std::shared_ptr<DeviceOwner> device,
               std::vector<std::shared_ptr<const GeometryMesh>> meshes,
               GeometrySceneKind kind)
        : device_(std::move(device)),
          meshes_(std::move(meshes)),
          buildPolicy_(ResolveGeometrySceneBuildPolicy(kind)) {}

    ~SceneOwner() {
        if (scene_ != nullptr) {
            rtcReleaseScene(scene_);
        }
    }

    SceneOwner(const SceneOwner&) = delete;
    SceneOwner& operator=(const SceneOwner&) = delete;

    bool Build() {
        if (!device_ || device_->Get() == nullptr) {
            return false;
        }

        rtcGetDeviceError(device_->Get());
        scene_ = rtcNewScene(device_->Get());
        if (scene_ == nullptr) {
            return false;
        }

        rtcSetSceneBuildQuality(
            scene_,
            buildPolicy_.lowBuildQuality
                ? RTC_BUILD_QUALITY_LOW
                : RTC_BUILD_QUALITY_MEDIUM);
        unsigned int sceneFlags = RTC_SCENE_FLAG_NONE;
        if (buildPolicy_.dynamicScene) {
            sceneFlags |= RTC_SCENE_FLAG_DYNAMIC;
        }
        if (buildPolicy_.robust) {
            sceneFlags |= RTC_SCENE_FLAG_ROBUST;
        }
        if (buildPolicy_.compact) {
            sceneFlags |= RTC_SCENE_FLAG_COMPACT;
        }
        rtcSetSceneFlags(scene_, static_cast<RTCSceneFlags>(sceneFlags));

        geometryById_.reserve(meshes_.size());
        for (const auto& mesh : meshes_) {
            if (!mesh || mesh->vertices.size() < 3 ||
                mesh->indices.size() < 3 ||
                (mesh->indices.size() % 3) != 0) {
                return false;
            }

            rtcGetDeviceError(device_->Get());
            RTCGeometry geometry =
                rtcNewGeometry(device_->Get(), RTC_GEOMETRY_TYPE_TRIANGLE);
            if (geometry == nullptr) {
                rtcGetDeviceError(device_->Get());
                return false;
            }

            auto* vertexBuffer = static_cast<GeometryPoint*>(
                rtcSetNewGeometryBuffer(
                    geometry, RTC_BUFFER_TYPE_VERTEX, 0,
                    RTC_FORMAT_FLOAT3, sizeof(GeometryPoint),
                    mesh->vertices.size()));
            if (vertexBuffer == nullptr) {
                rtcGetDeviceError(device_->Get());
                rtcReleaseGeometry(geometry);
                return false;
            }
            std::memcpy(vertexBuffer, mesh->vertices.data(),
                        mesh->vertices.size() * sizeof(GeometryPoint));

            auto* indexBuffer = static_cast<std::uint32_t*>(
                rtcSetNewGeometryBuffer(
                    geometry, RTC_BUFFER_TYPE_INDEX, 0,
                    RTC_FORMAT_UINT3, sizeof(std::uint32_t) * 3,
                    mesh->indices.size() / 3));
            if (indexBuffer == nullptr) {
                rtcGetDeviceError(device_->Get());
                rtcReleaseGeometry(geometry);
                return false;
            }
            std::memcpy(indexBuffer, mesh->indices.data(),
                        mesh->indices.size() * sizeof(std::uint32_t));
            rtcCommitGeometry(geometry);
            if (rtcGetDeviceError(device_->Get()) != RTC_ERROR_NONE) {
                rtcReleaseGeometry(geometry);
                return false;
            }
            const unsigned int geometryId = rtcAttachGeometry(scene_, geometry);
            rtcReleaseGeometry(geometry);
            const RTCError attachError = rtcGetDeviceError(device_->Get());
            if (geometryId == RTC_INVALID_GEOMETRY_ID ||
                attachError != RTC_ERROR_NONE) {
                if (geometryId != RTC_INVALID_GEOMETRY_ID) {
                    rtcDetachGeometry(scene_, geometryId);
                    rtcGetDeviceError(device_->Get());
                }
                return false;
            }
            const auto inserted = geometryById_.emplace(geometryId, mesh);
            if (!inserted.second) {
                rtcDetachGeometry(scene_, geometryId);
                rtcGetDeviceError(device_->Get());
                return false;
            }
        }

        if (geometryById_.size() != meshes_.size()) {
            return false;
        }

        rtcCommitScene(scene_);
        if (rtcGetDeviceError(device_->Get()) != RTC_ERROR_NONE) {
            rtcReleaseScene(scene_);
            scene_ = nullptr;
            return false;
        }
        return true;
    }

    RTCScene Get() const noexcept {
        return scene_;
    }

    std::size_t GeometryCount() const noexcept {
        return geometryById_.size();
    }

    std::shared_ptr<const GeometryMesh> FindMesh(
        unsigned int geometryId) const noexcept {
        const auto iterator = geometryById_.find(geometryId);
        return iterator == geometryById_.end() ? nullptr : iterator->second;
    }

private:
    std::shared_ptr<DeviceOwner> device_;
    std::vector<std::shared_ptr<const GeometryMesh>> meshes_;
    std::unordered_map<unsigned int, std::shared_ptr<const GeometryMesh>>
        geometryById_;
    GeometrySceneBuildPolicy buildPolicy_{};
    RTCScene scene_ = nullptr;
};

struct PublishedState {
    std::shared_ptr<const GeometrySnapshot> snapshot;
    std::shared_ptr<const SceneOwner> staticScene;
    std::shared_ptr<const SceneOwner> dynamicScene;
    std::vector<std::uintptr_t> sourceScenes;
};

enum class PublishResult : std::uint8_t {
    Published,
    Stale,
    Failed,
};

}  // namespace

struct GeometryRuntime::Impl final {
    explicit Impl() {
        auto initialSnapshot = std::make_shared<GeometrySnapshot>();
        auto initialState = std::make_shared<PublishedState>();
        initialState->snapshot = std::move(initialSnapshot);
        std::atomic_store_explicit(
            &published, std::shared_ptr<const PublishedState>(initialState),
            std::memory_order_release);
    }

    ~Impl() {
        Stop();
    }

    bool Start(ReadCallback callback, GeometryRuntimeConfig requestedConfig) {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        StopLocked();

        requestedConfig = Sanitize(std::move(requestedConfig));
        if (!callback ||
            (requestedConfig.instanceAddress == 0 &&
             requestedConfig.instancePointerSlots.empty())) {
            return false;
        }

        auto newDevice = std::make_shared<DeviceOwner>();
        if (newDevice->Get() == nullptr) {
            return false;
        }

        read = std::move(callback);
        config = std::move(requestedConfig);
        device = std::move(newDevice);
        generation = 0;
        {
            std::lock_guard<std::mutex> waitLock(waitMutex);
            requestEpoch.fetch_add(1, std::memory_order_acq_rel);
            refreshRequested = true;
        }
        running.store(true, std::memory_order_release);
        try {
            worker = std::thread(&Impl::WorkerMain, this);
        } catch (...) {
            running.store(false, std::memory_order_release);
            {
                std::lock_guard<std::mutex> waitLock(waitMutex);
                refreshRequested = false;
            }
            read = {};
            config = {};
            device.reset();
            return false;
        }
        return true;
    }

    void Stop() noexcept {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        StopLocked();
    }

    void StopLocked() noexcept {
        running.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> waitLock(waitMutex);
            refreshRequested = false;
        }
        waitCondition.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
        read = {};
        config = {};
        device.reset();
        PublishUnavailable(
            0, requestEpoch.load(std::memory_order_acquire));
    }

    bool IsRunning() const noexcept {
        return running.load(std::memory_order_acquire);
    }

    std::uint64_t RequestRefresh() noexcept {
        {
            std::lock_guard<std::mutex> lock(waitMutex);
            const std::uint64_t epoch =
                requestEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (!running.load(std::memory_order_acquire)) {
                return epoch;
            }
            refreshRequested = true;
            waitCondition.notify_all();
            return epoch;
        }
    }

    std::shared_ptr<const GeometrySnapshot> GetSnapshot() const noexcept {
        const auto state =
            std::atomic_load_explicit(&published, std::memory_order_acquire);
        return state ? state->snapshot : nullptr;
    }

    GeometryVisibility Trace(const GeometryPoint& origin,
                             const GeometryPoint& target) const noexcept {
        if (!IsFinite(origin) || !IsFinite(target)) {
            return GeometryVisibility::Unavailable;
        }

        const auto state =
            std::atomic_load_explicit(&published, std::memory_order_acquire);
        if (!state || !state->snapshot || !state->snapshot->available) {
            return GeometryVisibility::Unavailable;
        }
        const bool hasStaticScene =
            state->staticScene && state->staticScene->Get() != nullptr;
        const bool hasDynamicScene =
            state->dynamicScene && state->dynamicScene->Get() != nullptr;
        if (!hasStaticScene && !hasDynamicScene) {
            return GeometryVisibility::Unavailable;
        }

        const Vec3 start{origin.x, origin.y, origin.z};
        const Vec3 end{target.x, target.y, target.z};
        const Vec3 delta = Subtract(end, start);
        const float distanceSquared = LengthSquared(delta);
        if (!IsFinite(distanceSquared) || distanceSquared <= 1.0e-6f) {
            return GeometryVisibility::Unavailable;
        }

        const float distance = std::sqrt(distanceSquared);
        const float endPadding = std::min(1.0f, distance * 0.01f);
        if (distance <= endPadding + 0.01f) {
            return GeometryVisibility::Visible;
        }

        const Vec3 direction = Multiply(delta, 1.0f / distance);
        if (!IsFinite(direction)) {
            return GeometryVisibility::Unavailable;
        }

        const auto occludedBy =
            [&](const std::shared_ptr<const SceneOwner>& scene) {
                if (!scene || scene->Get() == nullptr) {
                    return false;
                }
                RTCRay ray{};
                ray.org_x = start.x;
                ray.org_y = start.y;
                ray.org_z = start.z;
                ray.dir_x = direction.x;
                ray.dir_y = direction.y;
                ray.dir_z = direction.z;
                ray.tnear = std::min(0.05f, distance * 0.001f);
                ray.tfar = distance - endPadding;
                ray.mask = 0xFFFFFFFFU;
                ray.flags = 0;

                RTCOccludedArguments arguments;
                rtcInitOccludedArguments(&arguments);
                rtcOccluded1(scene->Get(), &ray, &arguments);
                return ray.tfar < 0.0f;
            };
        return occludedBy(state->staticScene) ||
                occludedBy(state->dynamicScene)
            ? GeometryVisibility::Occluded
            : GeometryVisibility::Visible;
    }

    GeometryVisibility TraceFullSegment(
        const GeometryPoint& origin,
        const GeometryPoint& target) const noexcept {
        if (!IsFinite(origin) || !IsFinite(target)) {
            return GeometryVisibility::Unavailable;
        }

        const auto state =
            std::atomic_load_explicit(&published, std::memory_order_acquire);
        if (!state || !state->snapshot || !state->snapshot->available ||
            !state->staticScene || state->staticScene->Get() == nullptr ||
            !state->dynamicScene || state->dynamicScene->Get() == nullptr) {
            return GeometryVisibility::Unavailable;
        }

        const Vec3 start{origin.x, origin.y, origin.z};
        const Vec3 delta{
            target.x - origin.x,
            target.y - origin.y,
            target.z - origin.z,
        };
        const float distanceSquared = LengthSquared(delta);
        if (!IsFinite(distanceSquared) || distanceSquared <= 1.0e-6f) {
            return GeometryVisibility::Visible;
        }
        const float distance = std::sqrt(distanceSquared);
        const Vec3 direction = Multiply(delta, 1.0f / distance);
        if (!IsFinite(direction)) {
            return GeometryVisibility::Unavailable;
        }

        const auto intersects =
            [&](const std::shared_ptr<const SceneOwner>& scene) {
                RTCRayHit rayHit{};
                rayHit.ray.org_x = start.x;
                rayHit.ray.org_y = start.y;
                rayHit.ray.org_z = start.z;
                rayHit.ray.dir_x = direction.x;
                rayHit.ray.dir_y = direction.y;
                rayHit.ray.dir_z = direction.z;
                rayHit.ray.tnear = 0.0f;
                rayHit.ray.tfar = distance;
                rayHit.ray.mask = 0xFFFFFFFFU;
                rayHit.ray.flags = 0;
                rayHit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
                rayHit.hit.primID = RTC_INVALID_GEOMETRY_ID;
                for (unsigned int& instanceId : rayHit.hit.instID) {
                    instanceId = RTC_INVALID_GEOMETRY_ID;
                }

                RTCIntersectArguments arguments;
                rtcInitIntersectArguments(&arguments);
                rtcIntersect1(scene->Get(), &rayHit, &arguments);
                return rayHit.hit.geomID != RTC_INVALID_GEOMETRY_ID;
            };
        return intersects(state->staticScene) ||
                intersects(state->dynamicScene)
            ? GeometryVisibility::Occluded
            : GeometryVisibility::Visible;
    }

    GeometryRaycastHit Raycast(const GeometryPoint& origin,
                               const GeometryPoint& target) const noexcept {
        GeometryRaycastHit result;
        if (!IsFinite(origin) || !IsFinite(target)) {
            return result;
        }

        const auto state =
            std::atomic_load_explicit(&published, std::memory_order_acquire);
        if (!state || !state->snapshot || !state->snapshot->available) {
            return result;
        }
        const bool hasStaticScene =
            state->staticScene && state->staticScene->Get() != nullptr;
        const bool hasDynamicScene =
            state->dynamicScene && state->dynamicScene->Get() != nullptr;
        if (!hasStaticScene && !hasDynamicScene) {
            return result;
        }

        const Vec3 start{origin.x, origin.y, origin.z};
        const Vec3 end{target.x, target.y, target.z};
        const Vec3 delta = Subtract(end, start);
        const float distanceSquared = LengthSquared(delta);
        if (!IsFinite(distanceSquared) || distanceSquared <= 1.0e-6f) {
            return result;
        }

        const float maximumDistance = std::sqrt(distanceSquared);
        const Vec3 direction = Multiply(delta, 1.0f / maximumDistance);
        if (!IsFinite(direction)) {
            return result;
        }

        const auto intersectScene =
            [&](const std::shared_ptr<const SceneOwner>& scene) {
                if (!scene || scene->Get() == nullptr) {
                    return;
                }
                RTCRayHit rayHit{};
                rayHit.ray.org_x = start.x;
                rayHit.ray.org_y = start.y;
                rayHit.ray.org_z = start.z;
                rayHit.ray.dir_x = direction.x;
                rayHit.ray.dir_y = direction.y;
                rayHit.ray.dir_z = direction.z;
                rayHit.ray.tnear =
                    std::min(0.05f, maximumDistance * 0.001f);
                rayHit.ray.tfar =
                    result.mesh ? result.distance : maximumDistance;
                rayHit.ray.mask = 0xFFFFFFFFU;
                rayHit.ray.flags = 0;
                rayHit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
                rayHit.hit.primID = RTC_INVALID_GEOMETRY_ID;
                for (unsigned int& instanceId : rayHit.hit.instID) {
                    instanceId = RTC_INVALID_GEOMETRY_ID;
                }

                RTCIntersectArguments arguments;
                rtcInitIntersectArguments(&arguments);
                rtcIntersect1(scene->Get(), &rayHit, &arguments);
                if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID ||
                    !IsFinite(rayHit.ray.tfar) || rayHit.ray.tfar < 0.0f ||
                    rayHit.ray.tfar > maximumDistance) {
                    return;
                }
                std::shared_ptr<const GeometryMesh> mesh =
                    scene->FindMesh(rayHit.hit.geomID);
                if (!mesh) {
                    return;
                }
                result.mesh = std::move(mesh);
                result.distance = rayHit.ray.tfar;
                result.position = GeometryPoint{
                    start.x + direction.x * result.distance,
                    start.y + direction.y * result.distance,
                    start.z + direction.z * result.distance};
                result.triangleIndex = rayHit.hit.primID;
            };
        intersectScene(state->staticScene);
        intersectScene(state->dynamicScene);
        return result;
    }

private:
    bool SafeRead(std::uintptr_t address, void* destination,
                  std::size_t size) const noexcept {
        if (destination == nullptr || !IsRemoteRange(address, size)) {
            return false;
        }
        try {
            return read && read(address, destination, size);
        } catch (...) {
            return false;
        }
    }

    template <typename T>
    bool ReadObject(std::uintptr_t address, T& destination) const noexcept {
        destination = {};
        return IsRemoteRange(address, sizeof(T), alignof(T) > 8 ? 8 : alignof(T)) &&
               SafeRead(address, &destination, sizeof(T));
    }

    template <typename T>
    bool ReadVector(std::uintptr_t address, std::size_t count,
                    std::vector<T>& destination) const {
        destination.clear();
        if (count == 0) {
            return true;
        }

        std::size_t byteCount = 0;
        if (!CheckedMultiply(count, sizeof(T), byteCount) ||
            !IsRemoteRange(address, byteCount,
                           alignof(T) > 8 ? 8 : alignof(T))) {
            return false;
        }

        destination.resize(count);
        std::size_t byteOffset = 0;
        while (byteOffset < byteCount) {
            const std::size_t chunk =
                std::min(kMaximumReadChunk, byteCount - byteOffset);
            if (!SafeRead(address + byteOffset,
                          reinterpret_cast<std::uint8_t*>(destination.data()) +
                              byteOffset,
                          chunk)) {
                destination.clear();
                return false;
            }
            byteOffset += chunk;
        }
        return true;
    }

    bool ValidateArray(const RemoteArray& array,
                       std::size_t maximumCount) const noexcept {
        if (array.count < 0 || array.capacity < 0 ||
            array.count > array.capacity ||
            static_cast<std::size_t>(array.count) > maximumCount ||
            static_cast<std::size_t>(array.capacity) > maximumCount) {
            return false;
        }
        if (array.count == 0) {
            return true;
        }
        std::size_t byteCount = 0;
        return CheckedMultiply(static_cast<std::size_t>(array.count),
                               sizeof(std::uintptr_t), byteCount) &&
               IsRemoteRange(array.data, byteCount, alignof(std::uintptr_t));
    }

    bool ReadPointerArray(const RemoteArray& array,
                          std::size_t maximumCount,
                          std::vector<std::uintptr_t>& output) const {
        if (!ValidateArray(array, maximumCount)) {
            output.clear();
            return false;
        }
        return ReadVector(array.data, static_cast<std::size_t>(array.count),
                          output);
    }

    bool ValidateInstance(std::uintptr_t instance,
                          RemoteArray* sceneArray = nullptr) const noexcept {
        if (!IsRemoteRange(instance, kSceneArrayOffset + sizeof(RemoteArray), 8)) {
            return false;
        }
        RemoteArray scenes{};
        if (!ReadObject(instance + kSceneArrayOffset, scenes) ||
            !ValidateArray(scenes, kMaximumScenes)) {
            return false;
        }
        if (sceneArray != nullptr) {
            *sceneArray = scenes;
        }
        return true;
    }

    bool ValidatePopulatedInstance(
        std::uintptr_t instance) const noexcept {
        RemoteArray scenes{};
        return ValidateInstance(instance, &scenes) && scenes.count > 0;
    }

    static void CanonicalizePointerSet(
        std::vector<std::uintptr_t>& values) {
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()),
                     values.end());
    }

    bool ReadStablePointerValues(
        const RemoteArray& array, std::size_t maximumCount,
        std::vector<std::uintptr_t>& output) const {
        std::vector<std::uintptr_t> firstPointers;
        std::vector<std::uintptr_t> secondPointers;
        if (!ReadPointerArray(array, maximumCount, firstPointers) ||
            !ReadPointerArray(array, maximumCount, secondPointers)) {
            output.clear();
            return false;
        }
        CanonicalizePointerSet(firstPointers);
        CanonicalizePointerSet(secondPointers);
        if (firstPointers != secondPointers) {
            output.clear();
            return false;
        }
        output = std::move(secondPointers);
        return true;
    }

    bool ReadStableRemotePointerArray(
        std::uintptr_t headerAddress, std::size_t maximumCount,
        std::vector<std::uintptr_t>& output) const {
        RemoteArray firstHeader{};
        RemoteArray secondHeader{};
        std::vector<std::uintptr_t> firstPointers;
        std::vector<std::uintptr_t> secondPointers;
        if (!ReadObject(headerAddress, firstHeader) ||
            !ReadPointerArray(firstHeader, maximumCount, firstPointers) ||
            !ReadObject(headerAddress, secondHeader) ||
            !RemoteArrayEqual(firstHeader, secondHeader) ||
            !ReadPointerArray(secondHeader, maximumCount,
                              secondPointers)) {
            output.clear();
            return false;
        }
        CanonicalizePointerSet(firstPointers);
        CanonicalizePointerSet(secondPointers);
        if (firstPointers != secondPointers) {
            output.clear();
            return false;
        }
        output = std::move(secondPointers);
        return true;
    }

    bool ReadScenePointers(
        std::uintptr_t instance,
        std::vector<std::uintptr_t>& scenes) const {
        RemoteArray firstHeader{};
        std::vector<std::uintptr_t> firstPointers;
        if (!ValidateInstance(instance, &firstHeader) ||
            !ReadPointerArray(firstHeader, kMaximumScenes,
                              firstPointers)) {
            scenes.clear();
            return false;
        }

        RemoteArray secondHeader{};
        std::vector<std::uintptr_t> secondPointers;
        if (!ValidateInstance(instance, &secondHeader) ||
            !RemoteArrayEqual(firstHeader, secondHeader) ||
            !ReadPointerArray(secondHeader, kMaximumScenes,
                              secondPointers)) {
            scenes.clear();
            return false;
        }

        CanonicalizePointerSet(firstPointers);
        CanonicalizePointerSet(secondPointers);
        if (firstPointers.empty() || firstPointers != secondPointers) {
            scenes.clear();
            return false;
        }
        scenes = std::move(secondPointers);
        return true;
    }

    bool ResolveInstance(std::uintptr_t& output) const noexcept {
        output = 0;
        if (config.instanceAddress != 0) {
            if (!ValidatePopulatedInstance(config.instanceAddress)) {
                return false;
            }
            output = config.instanceAddress;
            return true;
        }

        bool readAnySlot = false;
        bool allSlotsReadable = true;
        bool sawCandidate = false;
        for (const std::uintptr_t slot : config.instancePointerSlots) {
            std::uintptr_t candidate = 0;
            if (!ReadObject(slot, candidate)) {
                allSlotsReadable = false;
                continue;
            }
            readAnySlot = true;
            if (candidate == 0) {
                continue;
            }
            sawCandidate = true;
            if (ValidatePopulatedInstance(candidate)) {
                output = candidate;
                return true;
            }
        }
        return readAnySlot && allSlotsReadable && !sawCandidate;
    }

    CollectionReport CollectPrunerActorShapes(
        std::uintptr_t instance, GeometryBodyType bodyType,
        std::vector<ActorShape>& output,
        std::size_t initialShapeCount,
        const std::vector<std::uintptr_t>* expectedScenes = nullptr) const {
        CollectionReport report;
        output.clear();
        if (initialShapeCount > config.maxShapes) {
            return report;
        }

        std::vector<std::uintptr_t> scenes;
        if (!ReadScenePointers(instance, scenes)) {
            report.partial = true;
            return report;
        }
        if (expectedScenes != nullptr && scenes != *expectedScenes) {
            report.partial = true;
            return report;
        }

        report.complete = true;
        std::vector<RemotePrunerPayload> mergedPayloads;
        const std::size_t prunerIndex =
            bodyType == GeometryBodyType::Static ? kStaticPrunerIndex
                                                 : kDynamicPrunerIndex;
        const bool shapeBudgetExhausted =
            initialShapeCount == config.maxShapes;
        if (shapeBudgetExhausted) {
            report.complete = false;
            report.budgetLimited = true;
        }

        for (const std::uintptr_t scene : scenes) {
            if (!IsRemoteRange(
                    scene,
                    kSceneQueryManagerOffset +
                        (kDynamicPrunerIndex + 1) * kPrunerStride,
                    8)) {
                report.complete = false;
                report.partial = true;
                continue;
            }
            const std::uintptr_t prunerSlot =
                scene + kSceneQueryManagerOffset +
                prunerIndex * kPrunerStride;

            std::uint32_t timestampBefore = 0;
            if (!ReadObject(prunerSlot + 0x2C, timestampBefore)) {
                report.complete = false;
                report.partial = true;
                continue;
            }
            std::uintptr_t prunerBefore = 0;
            if (!ReadObject(prunerSlot, prunerBefore) ||
                !IsRemoteRange(
                    prunerBefore,
                    kPruningPoolOffset + sizeof(RemotePruningPool), 8)) {
                report.complete = false;
                report.partial = true;
                continue;
            }

            RemotePruningPool poolBefore{};
            if (!ReadObject(prunerBefore + kPruningPoolOffset,
                            poolBefore) ||
                poolBefore.objectCount > poolBefore.maximumObjectCount ||
                poolBefore.objectCount > kMaximumPrunerObjects ||
                (poolBefore.objectCount != 0 &&
                 !IsRemoteRange(
                      poolBefore.objects,
                      static_cast<std::size_t>(poolBefore.objectCount) *
                          sizeof(RemotePrunerPayload),
                      alignof(RemotePrunerPayload)))) {
                report.complete = false;
                report.partial = true;
                continue;
            }

            std::vector<RemotePrunerPayload> firstPayloads;
            std::vector<RemotePrunerPayload> secondPayloads;
            if (!shapeBudgetExhausted &&
                !ReadVector(
                    poolBefore.objects,
                    static_cast<std::size_t>(poolBefore.objectCount),
                    firstPayloads)) {
                report.complete = false;
                report.partial = true;
                continue;
            }
            if (!shapeBudgetExhausted &&
                bodyType == GeometryBodyType::Dynamic &&
                (!ReadVector(
                     poolBefore.objects,
                     static_cast<std::size_t>(poolBefore.objectCount),
                     secondPayloads) ||
                 !PrunerPayloadsEqual(firstPayloads,
                                      secondPayloads))) {
                report.complete = false;
                report.partial = true;
                continue;
            }

            std::uintptr_t prunerAfter = 0;
            RemotePruningPool poolAfter{};
            std::uint32_t timestampAfter = 0;
            if (!ReadObject(prunerSlot, prunerAfter) ||
                !IsRemoteRange(
                    prunerAfter,
                    kPruningPoolOffset + sizeof(RemotePruningPool), 8) ||
                !ReadObject(prunerAfter + kPruningPoolOffset,
                            poolAfter) ||
                !ReadObject(prunerSlot + 0x2C, timestampAfter) ||
                prunerAfter != prunerBefore ||
                !PruningPoolEqual(poolBefore, poolAfter) ||
                timestampAfter != timestampBefore) {
                report.complete = false;
                report.partial = true;
                continue;
            }
            report.sourceAvailable = true;
            if (shapeBudgetExhausted) {
                continue;
            }

            const auto& acceptedPayloads =
                bodyType == GeometryBodyType::Dynamic
                    ? secondPayloads
                    : firstPayloads;
            std::vector<RemotePrunerPayload> validPayloads;
            validPayloads.reserve(acceptedPayloads.size());
            for (const RemotePrunerPayload& payload : acceptedPayloads) {
                if (!IsRemoteRange(payload.actor,
                                   sizeof(RemoteBody), 8) ||
                    !IsRemoteRange(payload.shape,
                                   sizeof(RemoteShape), 8)) {
                    report.complete = false;
                    report.partial = true;
                    continue;
                }
                validPayloads.push_back(payload);
            }

            mergedPayloads.insert(mergedPayloads.end(),
                                  validPayloads.begin(),
                                  validPayloads.end());
            std::sort(mergedPayloads.begin(), mergedPayloads.end(),
                      PrunerPayloadLess);
            mergedPayloads.erase(
                std::unique(
                    mergedPayloads.begin(), mergedPayloads.end(),
                    [](const RemotePrunerPayload& left,
                       const RemotePrunerPayload& right) {
                        return left.actor == right.actor &&
                               left.shape == right.shape;
                    }),
                mergedPayloads.end());
            if (mergedPayloads.size() > kMaximumPrunerObjects) {
                mergedPayloads.resize(kMaximumPrunerObjects);
                report.complete = false;
                report.budgetLimited = true;
            }
        }

        if (!report.sourceAvailable) {
            report.complete = false;
        }
        std::size_t actorCount = 0;
        std::uintptr_t previousActor = 0;
        bool havePreviousActor = false;
        output.reserve(std::min(
            mergedPayloads.size(), config.maxShapes - initialShapeCount));
        for (const RemotePrunerPayload& payload : mergedPayloads) {
            if (!havePreviousActor || payload.actor != previousActor) {
                if (actorCount >= config.maxActors) {
                    report.complete = false;
                    report.budgetLimited = true;
                    break;
                }
                previousActor = payload.actor;
                havePreviousActor = true;
                ++actorCount;
            }
            if (output.size() >= config.maxShapes - initialShapeCount) {
                report.complete = false;
                report.budgetLimited = true;
                break;
            }
            output.push_back(
                ActorShape{payload.actor, payload.shape});
        }
        return report;
    }

    CollectionReport CollectLegacyActorShapes(
        std::uintptr_t instance, std::uint16_t actorType,
        std::vector<ActorShape>& output,
        std::size_t initialShapeCount,
        const std::vector<std::uintptr_t>* expectedScenes = nullptr) const {
        CollectionReport report;
        output.clear();
        if (initialShapeCount > config.maxShapes) {
            return report;
        }
        std::vector<std::uintptr_t> scenes;
        if (!ReadScenePointers(instance, scenes)) {
            return report;
        }
        if (expectedScenes != nullptr && scenes != *expectedScenes) {
            report.partial = true;
            return report;
        }

        report.complete = true;
        std::vector<std::uintptr_t> mergedActors;
        bool actorTableAvailable = false;
        for (const std::uintptr_t scene : scenes) {
            if (!IsRemoteRange(
                    scene, kActorArrayOffset + sizeof(RemoteArray), 8)) {
                report.complete = false;
                report.partial = true;
                continue;
            }
            std::vector<std::uintptr_t> sceneActors;
            if (!ReadStableRemotePointerArray(
                    scene + kActorArrayOffset,
                    kMaximumActorsPerScene, sceneActors)) {
                report.complete = false;
                report.partial = true;
                continue;
            }
            actorTableAvailable = true;
            mergedActors.insert(mergedActors.end(), sceneActors.begin(),
                                sceneActors.end());
            CanonicalizePointerSet(mergedActors);
            if (mergedActors.size() > kMaximumActorsPerScene) {
                mergedActors.resize(kMaximumActorsPerScene);
                report.complete = false;
                report.budgetLimited = true;
            }
        }

        if (initialShapeCount == config.maxShapes) {
            report.complete = false;
            report.budgetLimited = true;
            report.sourceAvailable = actorTableAvailable;
            return report;
        }
        if (mergedActors.size() > config.maxActors) {
            mergedActors.resize(config.maxActors);
            report.complete = false;
            report.budgetLimited = true;
        }

        std::vector<ActorShape> mergedShapes;
        for (const std::uintptr_t actor : mergedActors) {
            if (!IsRemoteRange(actor, 8 + 42, 8)) {
                report.complete = false;
                report.partial = true;
                continue;
            }

            std::array<std::uint8_t, 42> firstHeader{};
            std::array<std::uint8_t, 42> secondHeader{};
            if (!SafeRead(actor + 8, firstHeader.data(),
                          firstHeader.size()) ||
                !SafeRead(actor + 8, secondHeader.data(),
                          secondHeader.size()) ||
                firstHeader != secondHeader) {
                report.complete = false;
                report.partial = true;
                continue;
            }

            std::uint16_t concreteType = 0;
            std::uintptr_t shapeStorage = 0;
            std::uint16_t shapeCount = 0;
            std::memcpy(&concreteType, secondHeader.data(),
                        sizeof(concreteType));
            std::memcpy(&shapeStorage, secondHeader.data() + 32,
                        sizeof(shapeStorage));
            std::memcpy(&shapeCount, secondHeader.data() + 40,
                        sizeof(shapeCount));
            if (concreteType != actorType || shapeCount == 0) {
                continue;
            }
            if (shapeCount > kMaximumShapesPerActor) {
                report.complete = false;
                report.partial = true;
                continue;
            }

            std::vector<std::uintptr_t> actorShapes;
            if (shapeCount == 1) {
                actorShapes.push_back(shapeStorage);
            } else {
                const RemoteArray shapeArray{
                    shapeStorage,
                    static_cast<std::int32_t>(shapeCount),
                    static_cast<std::int32_t>(shapeCount)};
                if (!ReadStablePointerValues(
                        shapeArray, kMaximumShapesPerActor,
                        actorShapes)) {
                    report.complete = false;
                    report.partial = true;
                    continue;
                }
            }
            for (const std::uintptr_t shape : actorShapes) {
                if (!IsRemoteRange(shape, sizeof(RemoteShape), 8)) {
                    report.complete = false;
                    report.partial = true;
                    continue;
                }
                mergedShapes.push_back(ActorShape{actor, shape});
            }
            if (mergedShapes.size() > kMaximumPrunerObjects) {
                mergedShapes.resize(kMaximumPrunerObjects);
                report.complete = false;
                report.budgetLimited = true;
            }
        }

        const std::size_t remainingShapes =
            config.maxShapes - initialShapeCount;
        if (mergedShapes.size() > remainingShapes) {
            mergedShapes.resize(remainingShapes);
            report.complete = false;
            report.budgetLimited = true;
        }
        output = std::move(mergedShapes);
        if (output.empty()) {
            report.complete = false;
            return report;
        }
        report.sourceAvailable = true;
        return report;
    }

    CollectionReport CollectActorShapes(
        std::uintptr_t instance, GeometryBodyType bodyType,
        std::vector<ActorShape>& output,
        std::size_t initialShapeCount,
        const std::vector<std::uintptr_t>* expectedScenes = nullptr) const {
        const std::uint16_t concreteType =
            bodyType == GeometryBodyType::Static ? kStaticActorType
                                                 : kDynamicActorType;
        CollectionReport report = CollectLegacyActorShapes(
            instance, concreteType, output, initialShapeCount,
            expectedScenes);
        if (report.sourceAvailable) {
            return report;
        }

        return CollectPrunerActorShapes(
            instance, bodyType, output, initialShapeCount,
            expectedScenes);
    }

    bool LoadTriangleMesh(const ActorShape& reference,
                          GeometryBodyType bodyType,
                          const GeometryBodyData& body,
                          const GeometryShapeData& shape,
                          std::shared_ptr<const GeometryMesh>& output) const {
        output.reset();

        std::int32_t geometryType = -1;
        std::memcpy(&geometryType, shape.shapeCore.core.geometry.data.data(),
                    sizeof(geometryType));
        if (geometryType != kTriangleMeshGeometryType) {
            return false;
        }

        RemoteTriangleMeshGeometry geometry{};
        std::memcpy(&geometry, shape.shapeCore.core.geometry.data.data(),
                    sizeof(geometry));
        const std::uintptr_t meshAddress =
            reinterpret_cast<std::uintptr_t>(geometry.triangleMesh);
        if (geometry.type != kTriangleMeshGeometryType ||
            !IsRemoteRange(meshAddress, sizeof(RemoteTriangleMesh), 8)) {
            return false;
        }

        RemoteTriangleMesh sourceMesh{};
        if (!ReadObject(meshAddress, sourceMesh) ||
            (sourceMesh.concreteType != kTriangleMeshBvh33Type &&
             sourceMesh.concreteType != kTriangleMeshBvh34Type) ||
            sourceMesh.vertexCount < 3 || sourceMesh.triangleCount == 0 ||
            sourceMesh.vertexCount > config.maxVerticesPerMesh ||
            sourceMesh.triangleCount > config.maxTrianglesPerMesh) {
            return false;
        }

        std::size_t indexCount = 0;
        if (!CheckedMultiply(
                static_cast<std::size_t>(sourceMesh.triangleCount), 3,
                indexCount)) {
            return false;
        }

        const std::uintptr_t vertexAddress =
            reinterpret_cast<std::uintptr_t>(sourceMesh.vertices);
        const std::uintptr_t indexAddress =
            reinterpret_cast<std::uintptr_t>(sourceMesh.triangles);
        if (vertexAddress == 0 || indexAddress == 0) {
            return false;
        }

        Transform worldTransform{};
        if (!BuildWorldTransform(body, bodyType, shape, worldTransform)) {
            return false;
        }

        Vec3 meshScale{};
        Quat meshScaleRotation{};
        if (!Convert(geometry.scale, meshScale, meshScaleRotation)) {
            return false;
        }

        std::vector<RemoteVec3> remoteVertices;
        if (!ReadVector(vertexAddress, sourceMesh.vertexCount,
                        remoteVertices)) {
            return false;
        }

        auto mesh = std::make_shared<GeometryMesh>();
        mesh->bodyType = bodyType;
        mesh->actorAddress = reference.actor;
        mesh->shapeAddress = reference.shape;
        mesh->vertices.resize(remoteVertices.size());

        double centerX = 0.0;
        double centerY = 0.0;
        double centerZ = 0.0;
        for (std::size_t index = 0; index < remoteVertices.size(); ++index) {
            const auto& source = remoteVertices[index];
            const Vec3 local{source.x, source.y, source.z};
            if (!IsReasonable(local)) {
                return false;
            }
            const Vec3 scaled =
                ApplyMeshScale(local, meshScale, meshScaleRotation);
            const Vec3 world = Apply(worldTransform, scaled);
            if (!IsReasonable(world)) {
                return false;
            }
            mesh->vertices[index] = GeometryPoint{world.x, world.y, world.z};
            centerX += static_cast<double>(world.x);
            centerY += static_cast<double>(world.y);
            centerZ += static_cast<double>(world.z);
        }

        if ((sourceMesh.flags & kMeshHas16BitIndices) != 0) {
            std::vector<std::uint16_t> smallIndices;
            if (!ReadVector(indexAddress, indexCount, smallIndices)) {
                return false;
            }
            mesh->indices.assign(smallIndices.begin(), smallIndices.end());
        } else if (!ReadVector(indexAddress, indexCount, mesh->indices)) {
            return false;
        }

        for (const std::uint32_t index : mesh->indices) {
            if (index >= mesh->vertices.size()) {
                return false;
            }
        }

        std::vector<std::uint32_t> validIndices;
        validIndices.reserve(mesh->indices.size());
        for (std::size_t index = 0; index + 2 < mesh->indices.size();
             index += 3) {
            const std::uint32_t first = mesh->indices[index];
            const std::uint32_t second = mesh->indices[index + 1];
            const std::uint32_t third = mesh->indices[index + 2];
            if (first == second || second == third || first == third) {
                continue;
            }

            const auto& a = mesh->vertices[first];
            const auto& b = mesh->vertices[second];
            const auto& c = mesh->vertices[third];
            const Vec3 edgeA{b.x - a.x, b.y - a.y, b.z - a.z};
            const Vec3 edgeB{c.x - a.x, c.y - a.y, c.z - a.z};
            const float areaSquared = LengthSquared(Cross(edgeA, edgeB));
            if (!IsFinite(areaSquared) || areaSquared <= 1.0e-10f) {
                continue;
            }
            validIndices.push_back(first);
            validIndices.push_back(second);
            validIndices.push_back(third);
        }
        if (validIndices.empty()) {
            return false;
        }
        mesh->indices = std::move(validIndices);

        const double inverseCount =
            1.0 / static_cast<double>(mesh->vertices.size());
        mesh->center = GeometryPoint{
            static_cast<float>(centerX * inverseCount),
            static_cast<float>(centerY * inverseCount),
            static_cast<float>(centerZ * inverseCount)};
        if (!IsFinite(mesh->center)) {
            return false;
        }

        float maximumDistanceSquared = 0.0f;
        for (const auto& vertex : mesh->vertices) {
            const Vec3 delta{vertex.x - mesh->center.x,
                             vertex.y - mesh->center.y,
                             vertex.z - mesh->center.z};
            maximumDistanceSquared =
                std::max(maximumDistanceSquared, LengthSquared(delta));
        }
        if (!IsFinite(maximumDistanceSquared)) {
            return false;
        }
        mesh->boundsRadius = std::sqrt(maximumDistanceSquared);
        output = std::move(mesh);
        return true;
    }

    bool LoadPrimitiveMesh(const ActorShape& reference,
                           GeometryBodyType bodyType,
                           const GeometryBodyData& body,
                           const GeometryShapeData& shape,
                           std::int32_t geometryType,
                           std::shared_ptr<const GeometryMesh>& output) const {
        Transform worldTransform{};
        if (!BuildWorldTransform(body, bodyType, shape, worldTransform)) {
            return false;
        }

        std::vector<Vec3> vertices;
        std::vector<std::uint32_t> indices;
        const auto& geometry = shape.shapeCore.core.geometry.data;

        if (geometryType == kBoxGeometryType) {
            RemoteBoxGeometry box{};
            std::memcpy(&box, geometry.data(), sizeof(box));
            const Vec3 extents{box.halfExtents.x, box.halfExtents.y,
                               box.halfExtents.z};
            if (box.type != kBoxGeometryType ||
                !IsValidExtent(extents.x) || !IsValidExtent(extents.y) ||
                !IsValidExtent(extents.z)) {
                return false;
            }
            vertices = {
                {-extents.x, -extents.y, -extents.z},
                {extents.x, -extents.y, -extents.z},
                {extents.x, extents.y, -extents.z},
                {-extents.x, extents.y, -extents.z},
                {-extents.x, -extents.y, extents.z},
                {extents.x, -extents.y, extents.z},
                {extents.x, extents.y, extents.z},
                {-extents.x, extents.y, extents.z},
            };
            indices = {
                0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6,
                0, 4, 5, 0, 5, 1, 2, 6, 7, 2, 7, 3,
                0, 3, 7, 0, 7, 4, 1, 5, 6, 1, 6, 2,
            };
        } else if (geometryType == kSphereGeometryType) {
            RemoteSphereGeometry sphere{};
            std::memcpy(&sphere, geometry.data(), sizeof(sphere));
            if (sphere.type != kSphereGeometryType ||
                !IsValidExtent(sphere.radius)) {
                return false;
            }

            constexpr std::size_t ringWidth =
                static_cast<std::size_t>(kRoundSegments + 1);
            vertices.reserve(
                static_cast<std::size_t>(kSphereRings + 1) * ringWidth);
            for (int ring = 0; ring <= kSphereRings; ++ring) {
                const float phi =
                    kPi * static_cast<float>(ring) /
                    static_cast<float>(kSphereRings);
                const float axial = sphere.radius * std::cos(phi);
                const float ringRadius = sphere.radius * std::sin(phi);
                for (int segment = 0; segment <= kRoundSegments; ++segment) {
                    const float theta =
                        2.0f * kPi * static_cast<float>(segment) /
                        static_cast<float>(kRoundSegments);
                    vertices.push_back(
                        Vec3{axial, ringRadius * std::cos(theta),
                             ringRadius * std::sin(theta)});
                }
            }
            indices.reserve(
                static_cast<std::size_t>(kSphereRings) *
                static_cast<std::size_t>(kRoundSegments) * 6);
            for (int ring = 0; ring < kSphereRings; ++ring) {
                for (int segment = 0; segment < kRoundSegments; ++segment) {
                    const auto first = static_cast<std::uint32_t>(
                        static_cast<std::size_t>(ring) * ringWidth +
                        static_cast<std::size_t>(segment));
                    const auto second =
                        static_cast<std::uint32_t>(first + ringWidth);
                    indices.insert(indices.end(),
                                   {first, second, first + 1,
                                    first + 1, second, second + 1});
                }
            }
        } else if (geometryType == kCapsuleGeometryType) {
            RemoteCapsuleGeometry capsule{};
            std::memcpy(&capsule, geometry.data(), sizeof(capsule));
            if (capsule.type != kCapsuleGeometryType ||
                !IsValidExtent(capsule.radius) ||
                !IsValidExtent(capsule.halfHeight)) {
                return false;
            }

            std::vector<std::pair<float, float>> profile;
            profile.reserve(
                static_cast<std::size_t>(kCapsuleHemisphereRings * 2 + 2));
            for (int ring = 0; ring <= kCapsuleHemisphereRings; ++ring) {
                const float angle =
                    -0.5f * kPi +
                    0.5f * kPi * static_cast<float>(ring) /
                        static_cast<float>(kCapsuleHemisphereRings);
                profile.emplace_back(
                    -capsule.halfHeight + capsule.radius * std::sin(angle),
                    capsule.radius * std::cos(angle));
            }
            profile.emplace_back(capsule.halfHeight, capsule.radius);
            for (int ring = 1; ring <= kCapsuleHemisphereRings; ++ring) {
                const float angle =
                    0.5f * kPi * static_cast<float>(ring) /
                    static_cast<float>(kCapsuleHemisphereRings);
                profile.emplace_back(
                    capsule.halfHeight + capsule.radius * std::sin(angle),
                    capsule.radius * std::cos(angle));
            }

            const std::size_t ringWidth =
                static_cast<std::size_t>(kRoundSegments + 1);
            vertices.reserve(profile.size() * ringWidth);
            for (const auto& [axial, ringRadius] : profile) {
                for (int segment = 0; segment <= kRoundSegments; ++segment) {
                    const float theta =
                        2.0f * kPi * static_cast<float>(segment) /
                        static_cast<float>(kRoundSegments);
                    vertices.push_back(
                        Vec3{axial, ringRadius * std::cos(theta),
                             ringRadius * std::sin(theta)});
                }
            }
            indices.reserve((profile.size() - 1) *
                            static_cast<std::size_t>(kRoundSegments) * 6);
            for (std::size_t ring = 0; ring + 1 < profile.size(); ++ring) {
                for (int segment = 0; segment < kRoundSegments; ++segment) {
                    const auto first = static_cast<std::uint32_t>(
                        ring * ringWidth +
                        static_cast<std::size_t>(segment));
                    const auto second =
                        static_cast<std::uint32_t>(first + ringWidth);
                    indices.insert(indices.end(),
                                   {first, second, first + 1,
                                    first + 1, second, second + 1});
                }
            }
        } else {
            return false;
        }

        if (vertices.size() > config.maxVerticesPerMesh ||
            indices.size() / 3 > config.maxTrianglesPerMesh) {
            return false;
        }
        return FinalizeMesh(reference, bodyType, worldTransform, vertices,
                            indices, output);
    }

    bool LoadConvexMesh(const ActorShape& reference,
                        GeometryBodyType bodyType,
                        const GeometryBodyData& body,
                        const GeometryShapeData& shape,
                        std::shared_ptr<const GeometryMesh>& output) const {
        RemoteConvexMeshGeometry geometry{};
        std::memcpy(&geometry,
                    shape.shapeCore.core.geometry.data.data(),
                    sizeof(geometry));
        const std::uintptr_t meshAddress =
            reinterpret_cast<std::uintptr_t>(geometry.convexMesh);
        if (geometry.type != kConvexMeshGeometryType ||
            !IsRemoteRange(meshAddress, sizeof(RemoteConvexMesh), 8)) {
            return false;
        }

        RemoteConvexMesh sourceMesh{};
        if (!ReadObject(meshAddress, sourceMesh) ||
            sourceMesh.concreteType != kConvexMeshType ||
            sourceMesh.hull.vertexCount < 3 ||
            sourceMesh.hull.polygonCount == 0 ||
            sourceMesh.hull.vertexCount > config.maxVerticesPerMesh) {
            return false;
        }

        const std::uintptr_t polygonAddress =
            reinterpret_cast<std::uintptr_t>(sourceMesh.hull.polygons);
        const std::size_t polygonCount = sourceMesh.hull.polygonCount;
        const std::size_t vertexCount = sourceMesh.hull.vertexCount;
        std::vector<RemoteHullPolygon> polygons;
        if (!ReadVector(polygonAddress, polygonCount, polygons)) {
            return false;
        }

        std::size_t polygonBytes = 0;
        std::size_t vertexBytes = 0;
        std::size_t edgeBytes = 0;
        std::size_t vertexAdjacencyBytes = 0;
        const std::size_t edgeCount =
            sourceMesh.hull.edgeCountAndFlags & 0x7FFFU;
        if (!CheckedMultiply(polygonCount, sizeof(RemoteHullPolygon),
                             polygonBytes) ||
            !CheckedMultiply(vertexCount, sizeof(RemoteVec3), vertexBytes) ||
            !CheckedMultiply(edgeCount, 2, edgeBytes) ||
            !CheckedMultiply(vertexCount, 3, vertexAdjacencyBytes)) {
            return false;
        }

        std::size_t vertexOffset = polygonBytes;
        std::size_t indexOffset = 0;
        if (!CheckedAdd(vertexOffset, vertexBytes, indexOffset) ||
            !CheckedAdd(indexOffset, edgeBytes, indexOffset) ||
            !CheckedAdd(indexOffset, vertexAdjacencyBytes, indexOffset)) {
            return false;
        }
        if ((sourceMesh.hull.edgeCountAndFlags & 0x8000U) != 0) {
            std::size_t wideEdgeBytes = 0;
            if (!CheckedMultiply(edgeCount, sizeof(std::uint16_t) * 2,
                                 wideEdgeBytes) ||
                !CheckedAdd(indexOffset, wideEdgeBytes, indexOffset)) {
                return false;
            }
        }
        if (polygonBytes > kMaximumRemoteAddress - polygonAddress ||
            indexOffset > kMaximumRemoteAddress - polygonAddress) {
            return false;
        }
        const std::uintptr_t vertexAddress = polygonAddress + vertexOffset;
        const std::uintptr_t indexAddress = polygonAddress + indexOffset;

        std::size_t convexIndexCount = 0;
        for (const auto& polygon : polygons) {
            if (polygon.vertexCount < 3 ||
                !CheckedAdd(convexIndexCount, polygon.vertexCount,
                            convexIndexCount)) {
                return false;
            }
        }
        if (convexIndexCount < 3) {
            return false;
        }

        std::vector<RemoteVec3> remoteVertices;
        std::vector<std::uint8_t> convexIndices;
        if (!ReadVector(vertexAddress, vertexCount, remoteVertices) ||
            !ReadVector(indexAddress, convexIndexCount, convexIndices)) {
            return false;
        }

        Vec3 meshScale{};
        Quat meshScaleRotation{};
        if (!Convert(geometry.scale, meshScale, meshScaleRotation)) {
            return false;
        }
        std::vector<Vec3> vertices;
        vertices.reserve(remoteVertices.size());
        for (const auto& vertex : remoteVertices) {
            const Vec3 local{vertex.x, vertex.y, vertex.z};
            if (!IsReasonable(local)) {
                return false;
            }
            const Vec3 scaled =
                ApplyMeshScale(local, meshScale, meshScaleRotation);
            if (!IsReasonable(scaled)) {
                return false;
            }
            vertices.push_back(scaled);
        }

        std::vector<std::uint32_t> indices;
        for (const auto& polygon : polygons) {
            const std::size_t start = polygon.vertexReference;
            const std::size_t count = polygon.vertexCount;
            if (start > convexIndices.size() ||
                count > convexIndices.size() - start) {
                return false;
            }
            if (count - 2 >
                config.maxTrianglesPerMesh - indices.size() / 3) {
                return false;
            }
            for (std::size_t offset = 1; offset + 1 < count; ++offset) {
                indices.push_back(convexIndices[start]);
                indices.push_back(convexIndices[start + offset]);
                indices.push_back(convexIndices[start + offset + 1]);
            }
        }

        Transform worldTransform{};
        if (!BuildWorldTransform(body, bodyType, shape, worldTransform)) {
            return false;
        }
        return FinalizeMesh(reference, bodyType, worldTransform, vertices,
                            indices, output);
    }

    bool LoadHeightField(const ActorShape& reference,
                         GeometryBodyType bodyType,
                         const GeometryBodyData& body,
                         const GeometryShapeData& shape,
                         std::shared_ptr<const GeometryMesh>& output) const {
        RemoteHeightFieldGeometry geometry{};
        std::memcpy(&geometry,
                    shape.shapeCore.core.geometry.data.data(),
                    sizeof(geometry));
        const std::uintptr_t fieldAddress =
            reinterpret_cast<std::uintptr_t>(geometry.heightField);
        if (geometry.type != kHeightFieldGeometryType ||
            !IsRemoteRange(fieldAddress, sizeof(RemoteHeightField), 8) ||
            !IsValidExtent(geometry.heightScale) ||
            !IsValidExtent(geometry.rowScale) ||
            !IsValidExtent(geometry.columnScale)) {
            return false;
        }

        RemoteHeightField sourceField{};
        if (!ReadObject(fieldAddress, sourceField) ||
            sourceField.concreteType != kHeightFieldType ||
            sourceField.sampleStride != sizeof(RemoteHeightFieldSample) ||
            sourceField.data.format != 0 ||
            sourceField.data.rows < 2 || sourceField.data.columns < 2) {
            return false;
        }

        const std::size_t rows = sourceField.data.rows;
        const std::size_t columns = sourceField.data.columns;
        std::size_t sampleCount = 0;
        std::size_t cellCount = 0;
        std::size_t maximumTriangleCount = 0;
        if (!CheckedMultiply(rows, columns, sampleCount) ||
            sampleCount != sourceField.sampleCount ||
            sampleCount > config.maxVerticesPerMesh ||
            !CheckedMultiply(rows - 1, columns - 1, cellCount) ||
            !CheckedMultiply(cellCount, 2, maximumTriangleCount) ||
            maximumTriangleCount > config.maxTrianglesPerMesh) {
            return false;
        }

        const std::uintptr_t sampleAddress =
            reinterpret_cast<std::uintptr_t>(sourceField.data.samples);
        std::vector<RemoteHeightFieldSample> samples;
        if (!ReadVector(sampleAddress, sampleCount, samples)) {
            return false;
        }

        std::vector<Vec3> vertices;
        vertices.reserve(sampleCount);
        for (std::size_t row = 0; row < rows; ++row) {
            for (std::size_t column = 0; column < columns; ++column) {
                const auto& sample = samples[row * columns + column];
                const Vec3 vertex{
                    static_cast<float>(row) * geometry.rowScale,
                    static_cast<float>(sample.height) * geometry.heightScale,
                    static_cast<float>(column) * geometry.columnScale};
                if (!IsReasonable(vertex)) {
                    return false;
                }
                vertices.push_back(vertex);
            }
        }

        std::vector<std::uint32_t> indices;
        indices.reserve(maximumTriangleCount * 3);
        for (std::size_t row = 0; row + 1 < rows; ++row) {
            for (std::size_t column = 0; column + 1 < columns; ++column) {
                const std::size_t first = row * columns + column;
                const std::size_t nextRow = first + columns;
                const auto& sample = samples[first];
                const bool tessellated =
                    (sample.material0 & kHeightFieldTessellationFlag) != 0;
                const bool firstHole =
                    (sample.material0 & kHeightFieldMaterialMask) ==
                    kHeightFieldHoleMaterial;
                const bool secondHole =
                    (sample.material1 & kHeightFieldMaterialMask) ==
                    kHeightFieldHoleMaterial;

                const auto a = static_cast<std::uint32_t>(first);
                const auto b = static_cast<std::uint32_t>(nextRow);
                const auto c = static_cast<std::uint32_t>(first + 1);
                const auto d = static_cast<std::uint32_t>(nextRow + 1);
                if (tessellated) {
                    if (!firstHole) {
                        indices.insert(indices.end(), {a, b, d});
                    }
                    if (!secondHole) {
                        indices.insert(indices.end(), {a, d, c});
                    }
                } else {
                    if (!firstHole) {
                        indices.insert(indices.end(), {a, b, c});
                    }
                    if (!secondHole) {
                        indices.insert(indices.end(), {c, b, d});
                    }
                }
            }
        }
        if (indices.empty()) {
            output.reset();
            return true;
        }

        Transform worldTransform{};
        if (!BuildWorldTransform(body, bodyType, shape, worldTransform)) {
            return false;
        }
        return FinalizeMesh(reference, bodyType, worldTransform, vertices,
                            indices, output);
    }

    enum class ReferenceLoadStatus : std::uint8_t {
        Failed,
        Resolved,
    };

    ReferenceLoadStatus LoadReferenceMesh(
        const ActorShape& reference, GeometryBodyType bodyType,
        std::unordered_map<std::uintptr_t, GeometryBodyData>& bodyCache,
        std::unordered_set<std::uintptr_t>& unreadableBodies,
        std::shared_ptr<const GeometryMesh>& output) const {
        output.reset();
        GeometryShapeData shape{};
        RemoteShape remoteShape{};
        if (!ReadObject(reference.shape, remoteShape)) {
            return ReferenceLoadStatus::Failed;
        }
        shape.shapeCore = remoteShape.shapeCore;

        std::int32_t geometryType = -1;
        std::memcpy(&geometryType,
                    shape.shapeCore.core.geometry.data.data(),
                    sizeof(geometryType));
        if (geometryType == kPlaneGeometryType) {
            return ReferenceLoadStatus::Resolved;
        }
        if (geometryType < kSphereGeometryType ||
            geometryType >= kGeometryTypeCount) {
            return ReferenceLoadStatus::Failed;
        }
        float heightFieldRowScale = 0.0f;
        if (geometryType == kHeightFieldGeometryType) {
            RemoteHeightFieldGeometry heightField{};
            std::memcpy(&heightField,
                        shape.shapeCore.core.geometry.data.data(),
                        sizeof(heightField));
            heightFieldRowScale = heightField.rowScale;
        }
        if (!ShouldIncludeGeometryShape(
                bodyType,
                geometryType,
                shape.shapeCore.core.shapeFlags,
                shape.shapeCore.core.materialIndex,
                heightFieldRowScale)) {
            return ReferenceLoadStatus::Resolved;
        }

        auto bodyIterator = bodyCache.find(reference.actor);
        if (bodyIterator == bodyCache.end()) {
            if (unreadableBodies.find(reference.actor) !=
                unreadableBodies.end()) {
                return ReferenceLoadStatus::Failed;
            }
            GeometryBodyData body{};
            RemoteBody remoteBody{};
            if (!ReadObject(reference.actor, remoteBody)) {
                unreadableBodies.insert(reference.actor);
                return ReferenceLoadStatus::Failed;
            }
            body.rigid = remoteBody.rigid;
            bodyIterator =
                bodyCache.emplace(reference.actor, std::move(body)).first;
        }

        bool loaded = false;
        if (geometryType == kTriangleMeshGeometryType) {
            loaded = LoadTriangleMesh(reference, bodyType,
                                      bodyIterator->second, shape, output);
        } else if (geometryType == kConvexMeshGeometryType) {
            loaded = LoadConvexMesh(reference, bodyType,
                                    bodyIterator->second, shape, output);
        } else if (geometryType == kHeightFieldGeometryType) {
            loaded = LoadHeightField(reference, bodyType,
                                     bodyIterator->second, shape, output);
        } else {
            loaded = LoadPrimitiveMesh(reference, bodyType,
                                       bodyIterator->second, shape,
                                       geometryType, output);
        }
        return loaded ? ReferenceLoadStatus::Resolved
                      : ReferenceLoadStatus::Failed;
    }

    CollectionReport CollectMeshes(
        std::uintptr_t instance, GeometryBodyType bodyType,
        const std::vector<std::shared_ptr<const GeometryMesh>>& previous,
        ActorShapeMissMap& missingCounts,
        std::vector<std::shared_ptr<const GeometryMesh>>& output,
        std::size_t initialMeshCount = 0,
        std::size_t initialVertexCount = 0,
        std::size_t initialTriangleCount = 0,
        std::size_t initialShapeCount = 0,
        std::size_t* selectedShapeCount = nullptr,
        const std::vector<std::uintptr_t>* expectedScenes = nullptr) const {
        CollectionReport report;
        output.clear();
        if (selectedShapeCount != nullptr) {
            *selectedShapeCount = 0;
        }
        if (initialMeshCount > config.maxMeshes ||
            initialVertexCount > config.maxTotalVertices ||
            initialTriangleCount > config.maxTotalTriangles ||
            initialShapeCount > config.maxShapes) {
            return report;
        }

        std::vector<ActorShape> references;
        report = CollectActorShapes(instance, bodyType, references,
                                    initialShapeCount, expectedScenes);
        if (!report.sourceAvailable && previous.empty()) {
            return report;
        }
        std::stable_sort(references.begin(), references.end(),
                         ActorShapeLess);
        references.erase(
            std::unique(references.begin(), references.end()),
            references.end());
        if (references.size() > config.maxShapes - initialShapeCount) {
            report.complete = false;
            report.budgetLimited = true;
            references.resize(config.maxShapes - initialShapeCount);
        }

        ActorShapeSet observedShapes;
        observedShapes.reserve(references.size());
        observedShapes.insert(references.begin(), references.end());
        using MeshMap = std::unordered_map<
            ActorShape, std::shared_ptr<const GeometryMesh>, ActorShapeHash>;
        MeshMap previousByShape;
        previousByShape.reserve(previous.size());
        for (const auto& mesh : previous) {
            if (!mesh || mesh->bodyType != bodyType ||
                mesh->vertices.size() < 3 || mesh->indices.size() < 3 ||
                (mesh->indices.size() % 3) != 0) {
                continue;
            }
            previousByShape.emplace(
                ActorShape{mesh->actorAddress, mesh->shapeAddress}, mesh);
        }

        const bool retainUnobserved =
            !report.budgetLimited &&
            (!report.sourceAvailable || report.partial);
        if (!retainUnobserved) {
            missingCounts.clear();
        } else {
            for (auto iterator = missingCounts.begin();
                 iterator != missingCounts.end();) {
                if (previousByShape.find(iterator->first) ==
                    previousByShape.end()) {
                    iterator = missingCounts.erase(iterator);
                } else {
                    ++iterator;
                }
            }
        }

        std::vector<ActorShape> candidateKeys = references;
        if (retainUnobserved) {
            candidateKeys.reserve(references.size() +
                                  previousByShape.size());
            for (const auto& entry : previousByShape) {
                if (observedShapes.find(entry.first) ==
                    observedShapes.end()) {
                    candidateKeys.push_back(entry.first);
                }
            }
        }
        std::stable_sort(candidateKeys.begin(), candidateKeys.end(),
                         ActorShapeLess);
        candidateKeys.erase(
            std::unique(candidateKeys.begin(), candidateKeys.end()),
            candidateKeys.end());

        std::unordered_map<std::uintptr_t, GeometryBodyData> bodyCache;
        std::unordered_set<std::uintptr_t> unreadableBodies;
        std::size_t totalVertices = initialVertexCount;
        std::size_t totalTriangles = initialTriangleCount;
        std::size_t shapeCount = initialShapeCount + references.size();
        output.reserve(std::min(candidateKeys.size(),
                                config.maxMeshes - initialMeshCount));

        for (const ActorShape& reference : candidateKeys) {
            const bool observed =
                observedShapes.find(reference) != observedShapes.end();
            std::shared_ptr<const GeometryMesh> candidate;
            bool useRetained = !observed;
            if (observed) {
                const ReferenceLoadStatus loadStatus =
                    LoadReferenceMesh(reference, bodyType, bodyCache,
                                      unreadableBodies, candidate);
                if (loadStatus == ReferenceLoadStatus::Resolved) {
                    missingCounts.erase(reference);
                    if (!candidate) {
                        continue;
                    }
                } else {
                    report.complete = false;
                    report.partial = true;
                    useRetained = true;
                }
            }

            if (useRetained) {
                const auto previousIterator =
                    previousByShape.find(reference);
                if (previousIterator == previousByShape.end()) {
                    continue;
                }
                std::size_t& misses = missingCounts[reference];
                if (misses < std::numeric_limits<std::size_t>::max()) {
                    ++misses;
                }
                if (misses > kMissingMeshRetentionRounds) {
                    continue;
                }
                if (!observed && shapeCount >= config.maxShapes) {
                    report.complete = false;
                    report.budgetLimited = true;
                    continue;
                }
                candidate = previousIterator->second;
            }

            if (!candidate) {
                continue;
            }
            const auto previousIterator = previousByShape.find(reference);
            if (previousIterator != previousByShape.end()) {
                ReuseEquivalentGeometryMesh(
                    previousIterator->second, candidate,
                    IsSameGeometryMeshContent);
            }
            const std::size_t candidateTriangles =
                candidate->indices.size() / 3;
            if (output.size() >= config.maxMeshes - initialMeshCount ||
                candidate->vertices.size() >
                    config.maxTotalVertices - totalVertices ||
                candidateTriangles >
                    config.maxTotalTriangles - totalTriangles) {
                report.complete = false;
                report.budgetLimited = true;
                break;
            }
            if (!observed) {
                ++shapeCount;
            }
            totalVertices += candidate->vertices.size();
            totalTriangles += candidateTriangles;
            output.push_back(std::move(candidate));
        }

        if (selectedShapeCount != nullptr) {
            *selectedShapeCount = shapeCount - initialShapeCount;
        }
        return report;
    }

    void PublishUnavailable(std::uintptr_t instance,
                            std::uint64_t epoch) noexcept {
        try {
            auto snapshot = std::make_shared<GeometrySnapshot>();
            snapshot->instanceAddress = instance;
            snapshot->refreshEpoch = epoch;
            auto state = std::make_shared<PublishedState>();
            {
                std::lock_guard<std::mutex> lock(waitMutex);
                if (requestEpoch.load(std::memory_order_acquire) != epoch) {
                    return;
                }
                snapshot->generation = ++generation;
                state->snapshot = std::move(snapshot);
                std::atomic_store_explicit(
                    &published, std::shared_ptr<const PublishedState>(state),
                    std::memory_order_release);
            }
        } catch (...) {
        }
    }

    PublishResult Publish(
        std::uintptr_t instance,
        const std::vector<std::shared_ptr<const GeometryMesh>>& staticMeshes,
        const std::vector<std::shared_ptr<const GeometryMesh>>& dynamicMeshes,
        bool rebuildStaticScene,
        const std::vector<std::uintptr_t>& expectedScenes,
        std::uint64_t epoch) {
        if (staticMeshes.empty() && dynamicMeshes.empty()) {
            return PublishResult::Failed;
        }
        if (staticMeshes.size() > config.maxMeshes ||
            dynamicMeshes.size() > config.maxMeshes - staticMeshes.size()) {
            return PublishResult::Failed;
        }

        auto snapshot = std::make_shared<GeometrySnapshot>();
        snapshot->available = true;
        snapshot->instanceAddress = instance;
        snapshot->refreshEpoch = epoch;
        snapshot->staticMeshes = staticMeshes;
        snapshot->dynamicMeshes = dynamicMeshes;

        std::size_t totalVertices = 0;
        const auto countMeshes =
            [&](const std::vector<std::shared_ptr<const GeometryMesh>>& meshes) {
                for (const auto& mesh : meshes) {
                    if (!mesh || mesh->vertices.size() < 3 ||
                        mesh->indices.size() < 3 ||
                        (mesh->indices.size() % 3) != 0) {
                        return false;
                    }
                    const std::size_t triangles = mesh->indices.size() / 3;
                    if (mesh->vertices.size() >
                            config.maxTotalVertices - totalVertices ||
                        triangles >
                            config.maxTotalTriangles -
                                snapshot->triangleCount) {
                        return false;
                    }
                    totalVertices += mesh->vertices.size();
                    snapshot->triangleCount += triangles;
                }
                return true;
            };
        if (!countMeshes(staticMeshes) || !countMeshes(dynamicMeshes) ||
            requestEpoch.load(std::memory_order_acquire) != epoch) {
            return requestEpoch.load(std::memory_order_acquire) != epoch
                ? PublishResult::Stale
                : PublishResult::Failed;
        }

        std::shared_ptr<const SceneOwner> staticScene;
        if (!rebuildStaticScene) {
            const auto current =
                std::atomic_load_explicit(
                    &published, std::memory_order_acquire);
            if (current && current->snapshot &&
                current->snapshot->available &&
                current->snapshot->instanceAddress == instance) {
                staticScene = current->staticScene;
            }
        }
        if (!staticScene) {
            auto candidate = std::make_shared<SceneOwner>(
                device, staticMeshes, GeometrySceneKind::Static);
            if (!candidate->Build()) {
                return PublishResult::Failed;
            }
            staticScene = std::move(candidate);
        }

        std::shared_ptr<const SceneOwner> dynamicScene;
        const auto current =
            std::atomic_load_explicit(&published, std::memory_order_acquire);
        // Mesh identity is conservative because immutable meshes contain
        // world-space vertices with the source transform already applied.
        if (current && current->snapshot && current->snapshot->available &&
            current->dynamicScene &&
            CanReuseGeometryScene(
                current->snapshot->instanceAddress,
                current->sourceScenes,
                current->snapshot->dynamicMeshes,
                instance,
                expectedScenes,
                dynamicMeshes)) {
            dynamicScene = current->dynamicScene;
        }
        if (!dynamicScene) {
            auto candidate = std::make_shared<SceneOwner>(
                device, dynamicMeshes, GeometrySceneKind::Dynamic);
            if (!candidate->Build()) {
                return PublishResult::Failed;
            }
            dynamicScene = std::move(candidate);
        }
        if (staticScene->GeometryCount() +
                dynamicScene->GeometryCount() ==
            0) {
            return PublishResult::Failed;
        }

        auto state = std::make_shared<PublishedState>();
        state->staticScene = std::move(staticScene);
        state->dynamicScene = std::move(dynamicScene);
        state->sourceScenes = expectedScenes;
        {
            std::lock_guard<std::mutex> lock(waitMutex);
            if (requestEpoch.load(std::memory_order_acquire) != epoch) {
                return PublishResult::Stale;
            }
            std::vector<std::uintptr_t> verifiedScenes;
            if (!ReadScenePointers(instance, verifiedScenes) ||
                verifiedScenes != expectedScenes) {
                return PublishResult::Failed;
            }
            snapshot->generation = ++generation;
            state->snapshot = std::move(snapshot);
            std::atomic_store_explicit(
                &published, std::shared_ptr<const PublishedState>(state),
                std::memory_order_release);
        }
        return PublishResult::Published;
    }

    void EnsureUnavailable(std::uintptr_t instance,
                           std::uint64_t epoch) noexcept {
        const auto state =
            std::atomic_load_explicit(&published, std::memory_order_acquire);
        if (state && state->snapshot && !state->snapshot->available &&
            state->snapshot->instanceAddress == instance &&
            state->snapshot->refreshEpoch >= epoch) {
            return;
        }
        PublishUnavailable(instance, epoch);
    }

    void WorkerMain() noexcept {
#if defined(__ANDROID__)
        setpriority(PRIO_PROCESS, 0, 10);
#endif
        std::vector<std::shared_ptr<const GeometryMesh>> staticMeshes;
        std::vector<std::shared_ptr<const GeometryMesh>> dynamicMeshes;
        ActorShapeMissMap staticMissingCounts;
        ActorShapeMissMap dynamicMissingCounts;
        std::size_t staticShapeCount = 0;
        std::uintptr_t activeInstance = 0;
        std::vector<std::uintptr_t> activeScenePointers;
        auto nextStaticRefresh = std::chrono::steady_clock::time_point::min();
        auto lastGoodAt = std::chrono::steady_clock::time_point::min();
        auto lastPublishedAt =
            std::chrono::steady_clock::time_point::min();
        bool hasLastGood = false;
        bool hasPublishedScene = false;
        std::size_t consecutiveFailures = 0;

        while (running.load(std::memory_order_acquire)) {
            bool forceRefresh = false;
            std::uint64_t roundEpoch = 0;
            {
                std::lock_guard<std::mutex> lock(waitMutex);
                forceRefresh = refreshRequested;
                refreshRequested = false;
                roundEpoch =
                    requestEpoch.load(std::memory_order_acquire);
            }

            const auto recordFailure =
                [&](std::chrono::steady_clock::time_point now,
                    std::uintptr_t instance) {
                    if (requestEpoch.load(std::memory_order_acquire) !=
                        roundEpoch) {
                        return;
                    }
                    if (consecutiveFailures <
                        std::numeric_limits<std::size_t>::max()) {
                        ++consecutiveFailures;
                    }
                    const bool expired =
                        hasLastGood &&
                        now - lastGoodAt >= config.lastGoodTtl;
                    if (!hasLastGood || expired ||
                        consecutiveFailures >=
                            config.maxConsecutiveFailures) {
                        EnsureUnavailable(instance, roundEpoch);
                        staticMeshes.clear();
                        dynamicMeshes.clear();
                        staticMissingCounts.clear();
                        dynamicMissingCounts.clear();
                        staticShapeCount = 0;
                        hasLastGood = false;
                        lastGoodAt =
                            std::chrono::steady_clock::time_point::min();
                        hasPublishedScene = false;
                        lastPublishedAt =
                            std::chrono::steady_clock::time_point::min();
                    }
                    nextStaticRefresh = now + config.dynamicRefresh;
                };

            try {
                std::uintptr_t instance = 0;
                const bool instanceResolved = ResolveInstance(instance);
                const auto now = std::chrono::steady_clock::now();
                std::vector<std::uintptr_t> scenePointers;
                const bool scenesResolved =
                    instanceResolved &&
                    (instance == 0 ||
                     ReadScenePointers(instance, scenePointers));
                if (!instanceResolved || !scenesResolved) {
                    recordFailure(now, activeInstance);
                } else if (instance == 0) {
                    activeInstance = 0;
                    activeScenePointers.clear();
                    staticMeshes.clear();
                    dynamicMeshes.clear();
                    staticMissingCounts.clear();
                    dynamicMissingCounts.clear();
                    staticShapeCount = 0;
                    hasLastGood = false;
                    hasPublishedScene = false;
                    consecutiveFailures = 0;
                    nextStaticRefresh =
                        std::chrono::steady_clock::time_point::min();
                    lastPublishedAt =
                        std::chrono::steady_clock::time_point::min();
                    EnsureUnavailable(0, roundEpoch);
                } else {
                    const bool instanceChanged = instance != activeInstance;
                    const bool sceneSetChanged =
                        scenePointers != activeScenePointers;
                    const bool sourceChanged =
                        forceRefresh || instanceChanged || sceneSetChanged;
                    if (sourceChanged) {
                        activeInstance = instance;
                        activeScenePointers = scenePointers;
                        staticMeshes.clear();
                        dynamicMeshes.clear();
                        staticMissingCounts.clear();
                        dynamicMissingCounts.clear();
                        staticShapeCount = 0;
                        hasLastGood = false;
                        hasPublishedScene = false;
                        consecutiveFailures = 0;
                        nextStaticRefresh =
                            std::chrono::steady_clock::time_point::min();
                        lastPublishedAt =
                            std::chrono::steady_clock::time_point::min();
                        EnsureUnavailable(instance, roundEpoch);
                    }

                    const bool refreshStatic =
                        sourceChanged || now >= nextStaticRefresh;
                    if (!ShouldPublishGeometryUpdate(
                            hasPublishedScene, lastPublishedAt, now,
                            sourceChanged || refreshStatic)) {
                        const auto remaining =
                            kMinimumGeometryPublishInterval -
                            std::chrono::duration_cast<
                                std::chrono::milliseconds>(
                                now - lastPublishedAt);
                        std::unique_lock<std::mutex> lock(waitMutex);
                        waitCondition.wait_for(
                            lock, remaining, [this] {
                                return !running.load(
                                           std::memory_order_acquire) ||
                                    refreshRequested;
                            });
                        continue;
                    }
                    std::vector<std::shared_ptr<const GeometryMesh>>
                        candidateStatic = staticMeshes;
                    std::size_t candidateStaticShapeCount =
                        staticShapeCount;
                    CollectionReport staticReport;
                    bool rebuildStaticScene = false;

                    if (refreshStatic) {
                        std::vector<std::shared_ptr<const GeometryMesh>>
                            freshStatic;
                        std::size_t freshStaticShapeCount = 0;
                        staticReport = CollectMeshes(
                            instance, GeometryBodyType::Static, staticMeshes,
                            staticMissingCounts, freshStatic, 0, 0, 0, 0,
                            &freshStaticShapeCount,
                            &scenePointers);
                        candidateStatic = std::move(freshStatic);
                        candidateStaticShapeCount = freshStaticShapeCount;
                        rebuildStaticScene =
                            staticReport.sourceAvailable ||
                            candidateStatic.size() != staticMeshes.size();
                        if (!staticReport.sourceAvailable) {
                            nextStaticRefresh =
                                now + config.dynamicRefresh;
                        }
                    }

                    bool staticBudgetValid = true;
                    std::size_t staticVertexCount = 0;
                    std::size_t staticTriangleCount = 0;
                    for (const auto& mesh : candidateStatic) {
                        if (!mesh || mesh->vertices.size() < 3 ||
                            mesh->indices.size() < 3 ||
                            (mesh->indices.size() % 3) != 0 ||
                            mesh->vertices.size() >
                                config.maxTotalVertices -
                                    staticVertexCount ||
                            mesh->indices.size() / 3 >
                                config.maxTotalTriangles -
                                    staticTriangleCount) {
                            staticBudgetValid = false;
                            break;
                        }
                        staticVertexCount += mesh->vertices.size();
                        staticTriangleCount += mesh->indices.size() / 3;
                    }

                    std::vector<std::shared_ptr<const GeometryMesh>>
                        candidateDynamic;
                    CollectionReport dynamicReport;
                    if (staticBudgetValid) {
                        dynamicReport = CollectMeshes(
                            instance, GeometryBodyType::Dynamic,
                            dynamicMeshes, dynamicMissingCounts,
                            candidateDynamic, candidateStatic.size(),
                            staticVertexCount, staticTriangleCount,
                            candidateStaticShapeCount, nullptr,
                            &scenePointers);
                    }

                    PublishResult publishResult = PublishResult::Failed;
                    const bool anySourceAvailable =
                        (refreshStatic && staticReport.sourceAvailable) ||
                        dynamicReport.sourceAvailable;
                    std::vector<std::uintptr_t> verifiedScenePointers;
                    const bool sceneSetStable =
                        ReadScenePointers(instance,
                                          verifiedScenePointers) &&
                        verifiedScenePointers == scenePointers;
                    if (requestEpoch.load(std::memory_order_acquire) !=
                        roundEpoch) {
                        publishResult = PublishResult::Stale;
                    } else if (sceneSetStable && staticBudgetValid &&
                               anySourceAvailable) {
                        publishResult =
                            Publish(instance, candidateStatic,
                                    candidateDynamic, rebuildStaticScene,
                                    scenePointers, roundEpoch);
                    }

                    if (publishResult == PublishResult::Published) {
                        staticMeshes = std::move(candidateStatic);
                        dynamicMeshes = std::move(candidateDynamic);
                        staticShapeCount = candidateStaticShapeCount;
                        if (refreshStatic &&
                            staticReport.sourceAvailable) {
                            nextStaticRefresh =
                                !staticReport.partial &&
                                        (staticReport.complete ||
                                         staticReport.budgetLimited)
                                    ? now + config.staticRefresh
                                    : now + config.dynamicRefresh;
                        }
                        hasLastGood = true;
                        hasPublishedScene = true;
                        lastGoodAt = now;
                        lastPublishedAt = now;
                        consecutiveFailures = 0;
                        if ((refreshStatic &&
                             (!staticReport.complete ||
                              staticReport.partial)) ||
                            !dynamicReport.complete ||
                            dynamicReport.partial) {
                            nextStaticRefresh = std::min(
                                nextStaticRefresh,
                                now + config.dynamicRefresh);
                        }
                    } else if (publishResult == PublishResult::Failed) {
                        if (refreshStatic) {
                            nextStaticRefresh =
                                std::min(nextStaticRefresh,
                                         now + config.dynamicRefresh);
                        }
                        recordFailure(now, instance);
                    }
                }
            } catch (...) {
                const auto now = std::chrono::steady_clock::now();
                recordFailure(now, activeInstance);
            }

            std::unique_lock<std::mutex> lock(waitMutex);
            waitCondition.wait_for(
                lock, config.dynamicRefresh, [this] {
                    return !running.load(std::memory_order_acquire) ||
                           refreshRequested;
                });
        }
    }

    mutable std::mutex lifecycleMutex;
    mutable std::mutex waitMutex;
    std::condition_variable waitCondition;
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> requestEpoch{0};
    bool refreshRequested = false;

    ReadCallback read;
    GeometryRuntimeConfig config{};
    std::shared_ptr<DeviceOwner> device;
    std::uint64_t generation = 0;
    std::shared_ptr<const PublishedState> published;
};

GeometryRuntime::GeometryRuntime() : impl_(std::make_unique<Impl>()) {}

GeometryRuntime::~GeometryRuntime() = default;

bool GeometryRuntime::Start(ReadCallback read, GeometryRuntimeConfig config) {
    return impl_->Start(std::move(read), std::move(config));
}

void GeometryRuntime::Stop() noexcept {
    impl_->Stop();
}

bool GeometryRuntime::IsRunning() const noexcept {
    return impl_->IsRunning();
}

std::uint64_t GeometryRuntime::RequestRefresh() noexcept {
    return impl_->RequestRefresh();
}

std::shared_ptr<const GeometrySnapshot> GeometryRuntime::GetSnapshot() const
    noexcept {
    return impl_->GetSnapshot();
}

GeometryVisibility GeometryRuntime::Trace(const GeometryPoint& origin,
                                          const GeometryPoint& target) const
    noexcept {
    return impl_->Trace(origin, target);
}

GeometryVisibility GeometryRuntime::TraceFullSegment(
    const GeometryPoint& origin, const GeometryPoint& target) const noexcept {
    return impl_->TraceFullSegment(origin, target);
}

GeometryRaycastHit GeometryRuntime::Raycast(const GeometryPoint& origin,
                                            const GeometryPoint& target) const
    noexcept {
    return impl_->Raycast(origin, target);
}

}  // namespace lengjing::game::native
