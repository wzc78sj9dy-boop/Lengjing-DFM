//
// Created by binglen on 2025/6/8.
//

#ifndef PHYSX_TYPES_H
#define PHYSX_TYPES_H

#include <cstdint>
#include <functional>
#include <vector>
#include "../Physx/foundation/PxTransform.h"
#include "../Physx/foundation/PxVec3.h"
#include "../Physx/foundation/PxVec4.h"

// 读取接口由运行后端提供。
extern void vm_readv(long int address, void *buffer, size_t size);
template <typename T> T vm_read(long int address);

// FVector 和 FRotator 定义（与游戏引擎兼容）
struct FVector {
    float X;
    float Y;
    float Z;
    
    FVector() : X(0), Y(0), Z(0) {}
    FVector(float x, float y, float z) : X(x), Y(y), Z(z) {}
    
    // 运算符重载
    FVector operator+(const FVector& other) const {
        return FVector(X + other.X, Y + other.Y, Z + other.Z);
    }
    
    FVector operator-(const FVector& other) const {
        return FVector(X - other.X, Y - other.Y, Z - other.Z);
    }
    
    FVector operator*(float scalar) const {
        return FVector(X * scalar, Y * scalar, Z * scalar);
    }
    
    FVector operator/(float scalar) const {
        if (scalar != 0.0f) {
            return FVector(X / scalar, Y / scalar, Z / scalar);
        }
        return FVector();
    }
};

struct FRotator {
    float Pitch;
    float Yaw;
    float Roll;
    
    FRotator() : Pitch(0), Yaw(0), Roll(0) {}
    FRotator(float pitch, float yaw, float roll) : Pitch(pitch), Yaw(yaw), Roll(roll) {}
};

// PhysX 数据结构（用于存储 PhysX 实例信息）
struct PhysXData {
    uint64_t PhysxInstancePtr;
    
    PhysXData() : PhysxInstancePtr(0) {}
};

// 全局 PhysX 数据对象
extern PhysXData Data;

#define SQManager 0x2430
#define RigidActors 0x2568

typedef unsigned long u64;
typedef unsigned int u32;

enum class PxGeometryType : int32_t
{

    eSPHERE        = 0, // 球体
    ePLANE         = 1, // 平面
    eCAPSULE       = 2, // 胶囊体（圆柱 + 半球两端）
    eBOX           = 3, // 盒子（立方体或矩形体）
    eCONVEXMESH    = 4, // 凸包网格（凸面多边形网格）
    eTRIANGLEMESH  = 5, // 三角形网格（可凹多边形，常用于静态物体）
    eHEIGHTFIELD   = 6, // 高度图（用于地形碰撞）
    eGEOMETRY_COUNT, //!< internal use only!
    eINVALID = -1	 //!< internal use only!
};

struct FilterDataT
{
	uint32_t word0;
	uint32_t word1;
	uint32_t word2;
	uint32_t word3;
};

struct PrunerPayload
{
	uint64_t Shape;
	uint64_t Actor;
	bool operator==(const PrunerPayload& other) const {
		return Shape == other.Shape && Actor == other.Actor;
	}
	bool operator<(const PrunerPayload& other) const {
		return std::tie(Shape, Actor) < std::tie(other.Shape, other.Actor);
	}
};

struct PrunerPayloadHash {
	size_t operator()(const PrunerPayload& p) const {
		return std::hash<uint64_t>()(p.Shape) ^ (std::hash<uint64_t>()(p.Actor) << 1);
	}
};

struct Int64Hash {
	size_t operator()(const uint64_t& p) const {
		return std::hash<uint64_t>()(p);
	}
};

template <class T>
void hash_combine(size_t& seed, const T& v) {
	std::hash<T> hasher;
	seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

template <typename T>
struct ATArray
{
	uintptr_t base;
	int32_t count;
	int32_t max;

	std::vector<T> ToVec() const
	{
		if (!IsValid())
		{
			return {};
		}

		std::vector<T> vec;
		vec.resize(static_cast<size_t>(count));

		vm_readv(base, vec.data(), static_cast<size_t>(count) * sizeof(T));
		if (vec.empty())
		{
			return {};
		}

		return vec;
	}

	T operator[](size_t index) const
	{
		if (index >= static_cast<size_t>(count))
		{
			return T{};
		}
		return vm_read<T>(base + index * sizeof(T));
	}

	bool IsValid() const
	{
		return base && count > 0 && count <= max && max > 0;
	}

	size_t Size() const
	{
		return static_cast<size_t>(count);
	}

	size_t Capacity() const
	{
		return static_cast<size_t>(max);
	}

	class Iterator
	{
	private:
		const ATArray* array;
		size_t index;

	public:
		Iterator(const ATArray* arr, size_t idx) : array(arr), index(idx) {}

		T operator*() const { return (*array)[index]; }
		Iterator& operator++() { ++index; return *this; }
		bool operator!=(const Iterator& other) const { return index != other.index; }
	};

	Iterator begin() const { return Iterator(this, 0); }
	Iterator end() const { return Iterator(this, Size()); }
};

#endif
