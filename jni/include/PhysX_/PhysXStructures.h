//
// Created by binglen on 2025/6/8.
//

#ifndef PHYSX_STRUCTURES_H
#define PHYSX_STRUCTURES_H

#include "PhysXTypes.h"
#include "PhysXEnums.h"
#include <vector>
#include <memory>
#include <cmath>

struct PxsRigidCoreT
{
	alignas(16) physx::PxTransform mBodyToWorld alignas(16);
	PhysX::PxRigidBodyFlag Flags{};
	uint8_t m_idt_body_to_actor{};
	uint16_t m_solver_iteration_counts{};
};

struct BodyCoreT
{
	char mPad[0x10]{};
	alignas(16) PxsRigidCoreT mCore {};
	alignas(16) physx::PxTransform mBodyToActor;
};

struct BodyT
{
	char mPad[0x60]{};
	uint64_t mScene{};
	uint64_t mControlState{};
	uint64_t mStreamPtr{};
	BodyCoreT mRigid{};
};

struct PxActorT
{
	char mPad[0x8]{};
	PhysX::PxConcreteType mType{};
	PhysX::PxBaseFlag mBaseFlags{};
};

struct PxGeometryT
{
	PxGeometryType mType{};
};

struct PxBoxGeometry : PxGeometryT
{
	physx::PxVec3 mHalfExtents{};

	bool valid()
	{
		if (mType != PxGeometryType::eBOX) return false;
		if (mHalfExtents.x <= 0.0f || mHalfExtents.y <= 0.0f || mHalfExtents.z <= 0.0f) return false;
		return true;
	}
};

struct PxSphereGeometryT : PxGeometryT
{
	float mRadius{};

	bool valid()
	{
		if (mType != PxGeometryType::eSPHERE) return false;
		if (mRadius <= 0.f) return false;
		return true;
	}
};

struct PxCapsuleGeometryT : PxGeometryT
{
	float mRadius{};
	float mHalfHeight{};

	bool valid()
	{
		if (mType != PxGeometryType::eCAPSULE) return false;
		if (mRadius <= 0.f || mHalfHeight <= 0.f) return false;
		return true;
	}
};

struct PxMeshScale
{
public:
	physx::PxVec3 transform(const physx::PxVec3& v) const
	{
		return rotation.rotateInv(scale.multiply(rotation.rotate(v)));
	}

	physx::PxVec3 scale;
	physx::PxQuat rotation;
};

struct CenterExtentsT
{
	physx::PxVec3 mCenter{};
	physx::PxVec3 mExtents{};
};

struct PxPlaneT
{
	physx::PxVec3 n;
	float d;
	bool operator==(const PxPlaneT& p) const
	{
		return n == p.n && d == p.d;
	}
};

struct HullPolygonDataT
{
	PxPlaneT mPlane;
	uint16_t mVRef8;
	uint8_t mNbVerts;
	uint8_t mMinIndex;
};

enum PxEMPTY
{
	PxEmpty
};

template <typename storageType, storageType bitMask>
class PxBitAndDataT
{
public:
	PxBitAndDataT(const PxEMPTY) {}
	PxBitAndDataT() : mData(0) {}
	PxBitAndDataT(storageType data, bool bit = false)
	{
		mData = bit ? storageType(data | bitMask) : data;
	}

	operator storageType() const { return storageType(mData & ~bitMask); }
	void setBit() { mData |= bitMask; }
	void clearBit() { mData &= ~bitMask; }
	storageType isBitSet() const { return storageType(mData & bitMask); }

protected:
	storageType mData;
};

typedef PxBitAndDataT<unsigned char, 0x80> PxBitAndByte;
typedef PxBitAndDataT<unsigned short, 0x8000> PxBitAndWord;
typedef PxBitAndDataT<unsigned int, 0x80000000> PxBitAndDword;

template<uint8_t TNumBytes>
struct PxPadding
{
	uint8_t mPadding[TNumBytes];
	PxPadding()
	{
		for (uint8_t idx = 0; idx < TNumBytes; ++idx)
			mPadding[idx] = 0;
	}
};

struct ConvexHullDataT
{
	CenterExtentsT mAABB{};
	physx::PxVec3 m_center_of_mass{};
	PxBitAndWord mNbEdges{};
	uint8_t HullVerticesNb{};
	uint8_t PolygonsNb{};
	HullPolygonDataT* mPolygons{};

	const physx::PxVec3* getHullVertices() const
	{
		const char* tmp = reinterpret_cast<const char*>(mPolygons);
		tmp += sizeof(HullPolygonDataT) * PolygonsNb;
		return reinterpret_cast<const physx::PxVec3*>(tmp);
	}

	const uint8_t* getVertexData8() const
	{
		const char* tmp = reinterpret_cast<const char*>(mPolygons);
		tmp += sizeof(HullPolygonDataT) * PolygonsNb;
		tmp += sizeof(physx::PxVec3) * HullVerticesNb;
		tmp += sizeof(uint8_t) * mNbEdges * 2;
		tmp += sizeof(uint8_t) * HullVerticesNb * 3;
		if (mNbEdges.isBitSet())
			tmp += sizeof(uint16_t) * mNbEdges * 2;
		return reinterpret_cast<const uint8_t*>(tmp);
	}

	const uint8_t* getIndexBuffer() const
	{
		int64_t v1 = mNbEdges & 0x7FFF;
		const char* result = reinterpret_cast<const char*>(mPolygons) + 15 * HullVerticesNb + 2 * v1;
		if ((mNbEdges & 0x8000u) != 0)
			result += 4 * v1;
		return reinterpret_cast<const uint8_t*>(result);
	}

	const uint8_t* getFacesByEdges8() const
	{
		const char* tmp = reinterpret_cast<const char*>(mPolygons);
		tmp += 20 * PolygonsNb;
		tmp += 12 * HullVerticesNb;
		return reinterpret_cast<const uint8_t*>(tmp);
	}

	const uint8_t* getFacesByVertices8() const
	{
		const char* tmp = reinterpret_cast<const char*>(mPolygons);
		tmp += 20 * PolygonsNb;
		tmp += 12 * HullVerticesNb;
		tmp += 1 * mNbEdges * 2;
		return reinterpret_cast<const uint8_t*>(tmp);
	}
};

struct PxConvexMeshT {};
struct ConvexMeshT : PxConvexMeshT
{
	char mPad[0x8]{};
	PhysX::PxConcreteType mType{};
	PhysX::PxBaseFlag mBaseFlags{};
	uint64_t mRefCountableVfptr{};
	int32_t mRefCount{};
	ConvexHullDataT HullData{};
	uint32_t mNb{};
};

struct PxConvexMeshGeometryT : PxGeometryT
{
	PxMeshScale Scale{};
	ConvexMeshT* ConvexMesh{};
	float mMaxMargin{};
	uint8_t mMeshFlags{};
	PxPadding<3> paddingFromFlags;
};

struct TriangleMeshT
{
	char mPad[0x8]{};
	PhysX::PxConcreteType mType{};
	PhysX::PxBaseFlag mBaseFlags{};
	uint64_t mRefCountableVfptr{};
	int32_t mRefCount{};
	uint32_t mNbVertices{};
	uint32_t mNbTriangles{};
	physx::PxVec3* Vertices{};
	void* mTriangles{};
	CenterExtentsT mAABB{};
	uint8_t* mExtraTrigData{};
	float mGeomEpsilon{};
	uint8_t Flags{};
};

struct PxTriangleMeshGeometryT : PxGeometryT
{
	PxMeshScale Scale{};
	uint8_t mMeshFlags{};
	PxPadding<3> paddingFromFlags;
	TriangleMeshT* mTriangleMesh{};
};

struct PxHeightFieldSampleT
{
	int16_t mHeight{};
	PxBitAndByte mMaterialIndex0{};
	PxBitAndByte mMaterialIndex1{};
};

struct HeightFieldDataT
{
	CenterExtentsT mAABB{};
	uint32_t Rows{};
	uint32_t Columns{};
	float mRowLimit{};
	float mColumnLimit{};
	float mNbColumns{};
	PxHeightFieldSampleT* mSamples{};
	float mThickness{};
	float mConvexEdgeThreshold{};
	uint16_t mFlags{};
	uint8_t mFormat{};
};

struct HeightFieldT
{
	char mPad[0x8]{};
	PhysX::PxConcreteType mType{};
	PhysX::PxBaseFlag mBaseFlags{};
	uint64_t mRefCountableVfptr{};
	int32_t mRefCount{};
	HeightFieldDataT mData{};
	uint32_t mSampleStride{};
	uint32_t mNbSamples{};
	float mMinHeight{};
	float mMaxHeight{};
	int32_t mModifyCount{};
	void* mMeshFactory{};
};

struct PxHeightFieldGeometryT : PxGeometryT
{
	HeightFieldT* mHeightField{};
	float HeightScale{};
	float RowScale{};
	float ColumnScale{};
	int8_t Flags{};
	PxPadding<3> paddingFromFlags;
};

struct GeometryUnionT
{
	union
	{
		void* alignment;
		uint8_t box[16];
		uint8_t sphere[8];
		uint8_t capsule[12];
		uint8_t plane[4];
		uint8_t convex[64];
		uint8_t mesh[80];
		uint8_t heightfield[56];
		uint8_t invalid[4];
	} mGeometry;

	PxGeometryType getType() const { return reinterpret_cast<const PxGeometryT&>(mGeometry).mType; }
};

struct PxShapeCoreT
{
	alignas(16) physx::PxTransform transform;
	float contactOffset;
	uint8_t mShapeFlags;
	uint8_t mOwnsMaterialIdxMemory;
	uint16_t materialIndex;
	GeometryUnionT geometry;
};

struct ShapeCoreT
{
	FilterDataT QueryFilterData{};
	FilterDataT SimulationFilterData{};
	alignas(16) PxShapeCoreT mCore {};
	float mRestOffset{};

	PxGeometryType getGeometryType() const { return mCore.geometry.getType(); }
	const physx::PxTransform getShape2Actor() const { return mCore.transform; }
};

struct ShapeT
{
	char mPad[0x30]{};
	uint64_t mScene{};
	uint32_t mControlState{};
	uint64_t mStreamPtr{};
	ShapeCoreT ShapeCore{};
	const PxGeometryT& GetGeometry() const { return reinterpret_cast<const PxGeometryT&>(ShapeCore.mCore.geometry.mGeometry); }
};

struct PxBounds3
{
	physx::PxVec3 min, max;
};

struct PruningPoolT
{
	uint32_t mNbObjects;
	uint32_t mMaxNbObjects;
	PxBounds3* mWorldBoxes;
	PrunerPayload* mObjects;
};

struct PrunerExtT
{
	uint64_t mPruner;
	char mPad[0x24]{};
	uint32_t mTimestamp;
};

struct NpSceneT
{
	char mPad[SQManager]{};
	PrunerExtT exts[2];
};

struct ShapeDataT
{
	ShapeT Shape{};
	BodyT Actor{};
	PxGeometryType mType{};
	uint8_t m_shape_flags{};
	PxMeshScale Scale{};
	PrunerPayload UniqueKey;

	ConvexMeshT ConvexMesh{};
	std::vector<HullPolygonDataT> polygons{};
	std::vector<uint8_t> ConvexIndices{};

	std::vector<uint32_t> Indices{};
	std::vector<uint16_t> SmallIndices{};
	TriangleMeshT TriangleMesh{};

	std::vector<physx::PxVec3> Vertices{};

	std::vector<PxHeightFieldSampleT> mSamples{};
	HeightFieldT mHeightField{};
};

#endif