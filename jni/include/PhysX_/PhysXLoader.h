//
// Created by binglen on 2025/6/8.
//

#ifndef PHYSX_LOADER_H
#define PHYSX_LOADER_H

#include "PhysXStructures.h"
#include "PhysXMeshData.h"
#include "PhysX_Log.h"
#include <algorithm>
#include <numeric>
#include <set>
#include <unordered_map>
#include <cmath>

namespace PhysX
{
	template<typename T>
	bool AddReadVecScatterOptimized(uint64_t address, size_t size, std::vector<T>* vec, std::vector<T>& tempBuffer) {
		if (!size || !vec) return false;

		try {
			const size_t MAX_VECTOR_SIZE = 1000000;
			if (size > MAX_VECTOR_SIZE) {
				return false;
			}

			if (tempBuffer.capacity() < size) {
				tempBuffer.reserve(size * 2);
			}
			tempBuffer.resize(size);

		}
		catch (const std::bad_alloc& e) {
			return false;
		}
		catch (const std::exception& e) {
			return false;
		}

		const size_t CHUNK_SIZE = 4096;
		for (size_t offset = 0; offset < size; offset += CHUNK_SIZE) {
			size_t chunk_size = std::min(CHUNK_SIZE, size - offset);
			vm_readv(address + offset * sizeof(T), tempBuffer.data() + offset, chunk_size * sizeof(T));
			if (!tempBuffer.data()) {
                printf(COLOR_RED "[?] 读取内存失败: 地址=0x%llx, 大小=%zu\n" COLOR_RESET, address + offset * sizeof(T), chunk_size * sizeof(T));
				return false;
			}
		}

		vec->assign(tempBuffer.begin(), tempBuffer.end());
		return true;
	}

    template<typename T>
    std::vector<T> ReadVec(uintptr_t address, size_t size, size_t custom_type_size = NULL)
    {
        std::vector<T> temp{};

        if (!size)
            return temp;

        temp.resize(custom_type_size ? custom_type_size : size);

        if (PhysX_Read(address, &temp[0], custom_type_size ? custom_type_size : sizeof(T) * size))
            return temp;

        return temp;
    }

	template <class _Ptr>
	std::vector<TriangleMeshData> GetMeshDataOptimized(std::vector<PrunerPayload>& objects, _Ptr filter, bool isDynamic = false, bool autoTransform = true) {
		std::vector<ShapeDataT> ShapeDatas{};
		std::vector<TriangleMeshData> TriangleMeshDatas{};

		ShapeDatas.resize(objects.size());
		TriangleMeshDatas.reserve(objects.size());

		std::vector<physx::PxVec3> tempVertexBuffer;
		std::vector<uint32_t> tempIndexBuffer;
		std::vector<uint16_t> tempSmallIndexBuffer;
		std::vector<PxHeightFieldSampleT> tempSampleBuffer;
		std::vector<HullPolygonDataT> tempPolygonBuffer;
		std::vector<uint8_t> tempConvexIndexBuffer;

		for (size_t i = 0; i < objects.size(); i++)
		{
			auto obj = objects[i];
			ShapeDatas[i].UniqueKey = obj;
            vm_readv(obj.Shape, &ShapeDatas[i].Shape, sizeof(ShapeT));
            vm_readv(obj.Actor, &ShapeDatas[i].Actor, sizeof(BodyT));
		}

		for (auto& ShapeData : ShapeDatas)
		{
			ShapeData.m_shape_flags = ShapeData.Shape.ShapeCore.mCore.mShapeFlags;
			ShapeData.mType = ShapeData.Shape.ShapeCore.getGeometryType();
		}

		ShapeDatas.erase(std::remove_if(ShapeDatas.begin(), ShapeDatas.end(), filter), ShapeDatas.end());

		for (auto& ShapeData : ShapeDatas)
		{
			if (ShapeData.mType == PxGeometryType::eHEIGHTFIELD)
			{
				PxHeightFieldGeometryT field_geometry = (PxHeightFieldGeometryT&)ShapeData.Shape.GetGeometry();
                vm_readv((uint64_t)field_geometry.mHeightField, &ShapeData.mHeightField, sizeof(HeightFieldT));
			}
			else if (ShapeData.mType == PxGeometryType::eCONVEXMESH)
			{
				PxConvexMeshGeometryT convex_geometry = (PxConvexMeshGeometryT&)ShapeData.Shape.GetGeometry();
				ShapeData.Scale = convex_geometry.Scale;
                vm_readv((uint64_t)convex_geometry.ConvexMesh, &ShapeData.ConvexMesh, sizeof(ConvexMeshT));
			}
		}

		for (auto& shapeData : ShapeDatas)
		{
			if (shapeData.mType == PxGeometryType::eCONVEXMESH)
			{
				auto nbPolygons = shapeData.ConvexMesh.HullData.PolygonsNb;
				auto mPolygons = (uint64_t)shapeData.ConvexMesh.HullData.mPolygons;
				auto size = sizeof(HullPolygonDataT);

				shapeData.polygons.resize(nbPolygons);
				for (uint32_t i = 0; i < nbPolygons; i++) {
					shapeData.polygons[i] = vm_read<HullPolygonDataT>(mPolygons + (i * size));
				}
			}
		}

		for (auto& ShapeData : ShapeDatas)
		{
			if (ShapeData.mType == PxGeometryType::eCONVEXMESH)
			{
				uint32_t indices_number = std::accumulate(ShapeData.polygons.begin(), ShapeData.polygons.end(), 0u, [](uint32_t sum, const HullPolygonDataT& polygon) {
					return sum + polygon.mNbVerts;
                });
				ShapeData.ConvexIndices.reserve(indices_number);

				AddReadVecScatterOptimized<uint8_t>((uint64_t)ShapeData.ConvexMesh.HullData.getVertexData8(), indices_number, &ShapeData.ConvexIndices, tempConvexIndexBuffer);
			}
			else  if (ShapeData.mType == PxGeometryType::eTRIANGLEMESH)
			{
				auto& geometry = (PxTriangleMeshGeometryT&)ShapeData.Shape.GetGeometry();
				const PxTriangleMeshGeometryT& triangle_geometry = (PxTriangleMeshGeometryT&)ShapeData.Shape.GetGeometry();
				ShapeData.Scale = triangle_geometry.Scale;
				ShapeData.TriangleMesh = vm_read<TriangleMeshT>((uintptr_t)triangle_geometry.mTriangleMesh);
			}
		}

		for (auto& ShapeData : ShapeDatas)
		{
			if (ShapeData.mType == PxGeometryType::eTRIANGLEMESH)
			{
				bool has16BitIndices = (ShapeData.TriangleMesh.Flags & 2U) ? true : false;

				AddReadVecScatterOptimized<physx::PxVec3>((uint64_t)ShapeData.TriangleMesh.Vertices, ShapeData.TriangleMesh.mNbVertices, &ShapeData.Vertices, tempVertexBuffer);

				if (has16BitIndices)
				{
					AddReadVecScatterOptimized<uint16_t>((uint64_t)ShapeData.TriangleMesh.mTriangles, ShapeData.TriangleMesh.mNbTriangles * 3, &ShapeData.SmallIndices, tempSmallIndexBuffer);
				}
				else
				{
					AddReadVecScatterOptimized<uint32_t>((uint64_t)ShapeData.TriangleMesh.mTriangles, ShapeData.TriangleMesh.mNbTriangles * 3, &ShapeData.Indices, tempIndexBuffer);
				}
			}
			else if (ShapeData.mType == PxGeometryType::eHEIGHTFIELD)
			{
				const uint32_t nb = ShapeData.mHeightField.mNbSamples;
				AddReadVecScatterOptimized<PxHeightFieldSampleT>((uint64_t)ShapeData.mHeightField.mData.mSamples, nb, &ShapeData.mSamples, tempSampleBuffer);
			}
			else if (ShapeData.mType == PxGeometryType::eCONVEXMESH)
			{
				AddReadVecScatterOptimized<physx::PxVec3>((uint64_t)ShapeData.ConvexMesh.HullData.getHullVertices(), (size_t)ShapeData.ConvexMesh.HullData.HullVerticesNb, &ShapeData.Vertices, tempVertexBuffer);
			}
		}

		for (int i = 0; i < ShapeDatas.size(); i++)
		{
			auto& ShapeData = ShapeDatas[i];
			if (ShapeData.mType == PxGeometryType::eTRIANGLEMESH && !ShapeData.SmallIndices.empty())
			{
				const size_t size = ShapeData.SmallIndices.size();
				ShapeData.Indices.resize(size);
				std::transform(ShapeData.SmallIndices.begin(), ShapeData.SmallIndices.end(), ShapeData.Indices.begin(), [](uint16_t val) { return static_cast<uint32_t>(val); });
				ShapeData.SmallIndices.clear();
				ShapeData.SmallIndices.shrink_to_fit();
			}
		}

		for (int i = 0; i < ShapeDatas.size(); i++)
		{
			const auto& ShapeDataItem = ShapeDatas[i];
			physx::PxTransform GlobalPose{};
			if (isDynamic) {
				GlobalPose = ShapeDataItem.Actor.mRigid.mCore.mBodyToWorld * ShapeDataItem.Actor.mRigid.mBodyToActor.getInverse();
			}
			else {
				GlobalPose = ShapeDataItem.Actor.mRigid.mCore.mBodyToWorld;
			}
			auto LocalPose = ShapeDataItem.Shape.ShapeCore.mCore.transform;
			auto CombinePose = GlobalPose * LocalPose;
			auto UniqueKey = ShapeDataItem.UniqueKey;

			if (ShapeDataItem.mType == PxGeometryType::eBOX) {
				const PxBoxGeometry& boxGeometry = (PxBoxGeometry&)ShapeDataItem.Shape.GetGeometry();
				auto halfExtents = boxGeometry.mHalfExtents;

				physx::PxVec3 vertices[8];
				vertices[0] = physx::PxVec3(-halfExtents.x, -halfExtents.y, -halfExtents.z);
				vertices[1] = physx::PxVec3(halfExtents.x, -halfExtents.y, -halfExtents.z);
				vertices[2] = physx::PxVec3(halfExtents.x, halfExtents.y, -halfExtents.z);
				vertices[3] = physx::PxVec3(-halfExtents.x, halfExtents.y, -halfExtents.z);
				vertices[4] = physx::PxVec3(-halfExtents.x, -halfExtents.y, halfExtents.z);
				vertices[5] = physx::PxVec3(halfExtents.x, -halfExtents.y, halfExtents.z);
				vertices[6] = physx::PxVec3(halfExtents.x, halfExtents.y, halfExtents.z);
				vertices[7] = physx::PxVec3(-halfExtents.x, halfExtents.y, halfExtents.z);

				if (autoTransform) {
					for (int i = 0; i < 8; i++)
					{
						vertices[i] = CombinePose.transform(vertices[i]);
					}
				}
				// Box indices for triangles
				uint32_t indices[] = {
					// Front face
					0, 1, 2,
					0, 2, 3,
					// Back face
					4, 6, 5,
					4, 7, 6,
					// Top face
					0, 4, 5,
					0, 5, 1,
					// Bottom face
					2, 6, 7,
					2, 7, 3,
					// Right face
					0, 3, 7,
					0, 7, 4,
					// Left face
					1, 5, 6,
					1, 6, 2 };

				TriangleMeshData mesh_data{};
				mesh_data.Vertices.assign(vertices, vertices + 8);
				mesh_data.Indices.assign(indices, indices + 36);
				mesh_data.Flags = ShapeDataItem.m_shape_flags;
				mesh_data.QueryFilterData = ShapeDataItem.Shape.ShapeCore.QueryFilterData;
				mesh_data.SimulationFilterData = ShapeDataItem.Shape.ShapeCore.SimulationFilterData;
				mesh_data.UniqueKey1 = UniqueKey;
				mesh_data.Type = PxGeometryType::eBOX;
				mesh_data.Transform = CombinePose;
				mesh_data.HashValue = mesh_data.ComputeHash();

				{
					TriangleMeshDatas.push_back(mesh_data);
				}
			}
            else if (ShapeDataItem.mType == PxGeometryType::eCAPSULE)
            {
                const PxCapsuleGeometryT& capsuleGeometry = (const PxCapsuleGeometryT&)ShapeDataItem.Shape.GetGeometry();
                float radius = capsuleGeometry.mRadius;
                float halfHeight = capsuleGeometry.mHalfHeight;

                TriangleMeshData MeshData{};

                int segments = 12;
                int hemisphereRings = 6;
                int cylinderRings = 3;

                std::vector<physx::PxVec3> vertices;

                for (int ring = 0; ring <= hemisphereRings; ring++) {
                    float phi = M_PI * 0.5f * ring / hemisphereRings;  // 0 到 π/2
                    float y = -halfHeight - radius * cos(phi);
                    float currentRadius = radius * sin(phi);

                    for (int seg = 0; seg <= segments; seg++) {
                        float theta = 2 * M_PI * seg / segments;
                        float x = currentRadius * cos(theta);
                        float z = currentRadius * sin(theta);
                        vertices.push_back(physx::PxVec3(x, y, z));
                    }
                }

                int cylinderStartIndex = vertices.size();
                for (int ring = 0; ring <= cylinderRings; ring++) {
                    float y = -halfHeight + (2 * halfHeight * ring) / cylinderRings;

                    for (int seg = 0; seg <= segments; seg++) {
                        float theta = 2 * M_PI * seg / segments;
                        float x = radius * cos(theta);
                        float z = radius * sin(theta);
                        vertices.push_back(physx::PxVec3(x, y, z));
                    }
                }

                int upperStartIndex = vertices.size();
                for (int ring = 0; ring <= hemisphereRings; ring++) {
                    float phi = M_PI * 0.5f * ring / hemisphereRings;  // 0 到 π/2
                    float y = halfHeight + radius * cos(phi);
                    float currentRadius = radius * sin(phi);

                    for (int seg = 0; seg <= segments; seg++) {
                        float theta = 2 * M_PI * seg / segments;
                        float x = currentRadius * cos(theta);
                        float z = currentRadius * sin(theta);
                        vertices.push_back(physx::PxVec3(x, y, z));
                    }
                }

                std::vector<uint32_t> indices;

                for (int ring = 0; ring < hemisphereRings; ring++) {
                    for (int seg = 0; seg < segments; seg++) {
                        int current = ring * (segments + 1) + seg;
                        int next = current + (segments + 1);

                        indices.insert(indices.end(), {static_cast<uint32_t>(current),static_cast<uint32_t>(next),static_cast<uint32_t>(current + 1)});
                        indices.insert(indices.end(), {static_cast<uint32_t>(current + 1),static_cast<uint32_t>(next),static_cast<uint32_t>(next + 1)});
                    }
                }

                for (int ring = 0; ring < cylinderRings; ring++) {
                    int ringStart = cylinderStartIndex + ring * (segments + 1);

                    for (int seg = 0; seg < segments; seg++) {
                        int current = ringStart + seg;
                        int next = current + (segments + 1);

                        indices.insert(indices.end(), {static_cast<uint32_t>(current),static_cast<uint32_t>(next),static_cast<uint32_t>(current + 1)
                        });
                        indices.insert(indices.end(), {static_cast<uint32_t>(current + 1),static_cast<uint32_t>(next),static_cast<uint32_t>(next + 1)
                        });
                    }
                }

                for (int ring = 0; ring < hemisphereRings; ring++) {
                    int ringStart = upperStartIndex + ring * (segments + 1);

                    for (int seg = 0; seg < segments; seg++) {
                        int current = ringStart + seg;
                        int next = current + (segments + 1);

                        indices.insert(indices.end(), {static_cast<uint32_t>(current),static_cast<uint32_t>(next),static_cast<uint32_t>(current + 1)
                        });
                        indices.insert(indices.end(), {static_cast<uint32_t>(current + 1),static_cast<uint32_t>(next),static_cast<uint32_t>(next + 1)
                        });
                    }
                }

                if (autoTransform) {
                    for (auto& vertex : vertices) {
                        vertex = CombinePose.transform(vertex);
                    }
                }

                MeshData.Vertices = vertices;
                MeshData.Indices = indices;
                MeshData.Flags = ShapeDataItem.m_shape_flags;
                MeshData.QueryFilterData = ShapeDataItem.Shape.ShapeCore.QueryFilterData;
                MeshData.SimulationFilterData = ShapeDataItem.Shape.ShapeCore.SimulationFilterData;
                MeshData.UniqueKey1 = UniqueKey;
                MeshData.Type = PxGeometryType::eCAPSULE;
                MeshData.Transform = CombinePose;
                MeshData.HashValue = MeshData.ComputeHash();

                TriangleMeshDatas.push_back(MeshData);
            }
			else if (ShapeDataItem.mType == PxGeometryType::eSPHERE)
			{
				const PxSphereGeometryT& sphereGeometry = (const PxSphereGeometryT&)ShapeDataItem.Shape.GetGeometry();
				float radius = sphereGeometry.mRadius;

				TriangleMeshData MeshData{};

				int segments = 16;
				int rings = 16;

				std::vector<physx::PxVec3> vertices;

				for (int ring = 0; ring <= rings; ring++) {
					float phi = M_PI * ring / rings;
					float y = radius * cos(phi);

					for (int seg = 0; seg <= segments; seg++) {
						float theta = 2 * M_PI * seg / segments;
						float x = radius * sin(phi) * cos(theta);
						float z = radius * sin(phi) * sin(theta);
						vertices.push_back(physx::PxVec3(x, y, z));
					}
				}

				std::vector<uint32_t> indices;

				for (int ring = 0; ring < rings; ring++) {
					for (int seg = 0; seg < segments; seg++) {
						int current = ring * (segments + 1) + seg;
						int next = current + (segments + 1);

						indices.insert(indices.end(), { static_cast<unsigned int>(current), static_cast<unsigned int>(next), static_cast<unsigned int>(current + 1) });
						indices.insert(indices.end(), { static_cast<unsigned int>(current + 1), static_cast<unsigned int>(next), static_cast<unsigned int>(next + 1) });
					}
				}

				if (autoTransform) {
					for (auto& vertex : vertices) {
						vertex = CombinePose.transform(vertex);
					}
				}

				MeshData.Vertices = vertices;
				MeshData.Indices = indices;
				MeshData.Flags = ShapeDataItem.m_shape_flags;
				MeshData.QueryFilterData = ShapeDataItem.Shape.ShapeCore.QueryFilterData;
				MeshData.SimulationFilterData = ShapeDataItem.Shape.ShapeCore.SimulationFilterData;
				MeshData.UniqueKey1 = UniqueKey;
				MeshData.Type = PxGeometryType::eSPHERE;
				MeshData.Transform = CombinePose;
				MeshData.HashValue = MeshData.ComputeHash();

				TriangleMeshDatas.push_back(MeshData);
			}
//            else if (ShapeDataItem.mType == PxGeometryType::eCAPSULE)
//            {
//                const PxCapsuleGeometryT& capsuleGeometry = (const PxCapsuleGeometryT&)ShapeDataItem.Shape.GetGeometry();
//                float radius = capsuleGeometry.mRadius;
//                float halfHeight = capsuleGeometry.mHalfHeight;
//
//            }
//            else if (ShapeDataItem.mType == PxGeometryType::eSPHERE)
//            {
//                const PxSphereGeometryT& sphereGeometry = (const PxSphereGeometryT&)ShapeDataItem.Shape.GetGeometry();
//                float radius = sphereGeometry.mRadius;
//                const PxMeshScale& ScaleVal = ShapeDataItem.Scale;
//
//            }
			else if (ShapeDataItem.mType == PxGeometryType::eTRIANGLEMESH)
			{
				TriangleMeshData MeshData{};
				PxMeshScale ScaleVal = ShapeDataItem.Scale;
				const size_t vertexCount = ShapeDataItem.Vertices.size();
				MeshData.Vertices.resize(vertexCount);
				for (size_t i = 0; i < vertexCount; i++)
				{
					physx::PxVec3& vertex = MeshData.Vertices[i];
					vertex = ScaleVal.transform(ShapeDataItem.Vertices[i]);
					if (autoTransform) {
						vertex = CombinePose.transform(vertex);
					}
				}
				MeshData.Indices = ShapeDataItem.Indices;
				MeshData.Flags = ShapeDataItem.m_shape_flags;
				MeshData.QueryFilterData = ShapeDataItem.Shape.ShapeCore.QueryFilterData;
				MeshData.SimulationFilterData = ShapeDataItem.Shape.ShapeCore.SimulationFilterData;
				MeshData.UniqueKey1 = UniqueKey;
				MeshData.Type = PxGeometryType::eTRIANGLEMESH;
				MeshData.Transform = CombinePose;
				MeshData.HashValue = MeshData.ComputeHash(); //
				//#pragma omp critical
				{
					TriangleMeshDatas.push_back(MeshData);
				}
			}
            else if (ShapeDataItem.mType == PxGeometryType::eHEIGHTFIELD)
            {
                PxHeightFieldGeometryT field_geometry = (PxHeightFieldGeometryT&)ShapeDataItem.Shape.GetGeometry();

                auto NumRows = ShapeDataItem.mHeightField.mData.Rows;        // 行数
                auto NumColumns = ShapeDataItem.mHeightField.mData.Columns;  // 列数
                auto ColumnScale = field_geometry.ColumnScale;              // 列缩放
                auto RowScale = field_geometry.RowScale;                    // 行缩放
                auto HeightScale = field_geometry.HeightScale;              // 高度缩放

                TriangleMeshData MeshData{};

                if (NumRows == 0 || NumColumns == 0 || ShapeDataItem.mSamples.empty()) {
                    // fix unsign int
                    continue;
                }

                MeshData.Vertices.reserve(NumRows * NumColumns);

                for (uint32_t row = 0; row < NumRows; row++) {
                    for (uint32_t col = 0; col < NumColumns; col++) {
                        uint32_t idx = row * NumColumns + col;
                        auto sample = ShapeDataItem.mSamples[idx];
                        physx::PxVec3 vertex = { row * RowScale, sample.mHeight * HeightScale, col * ColumnScale };

                        if (autoTransform) {
                            vertex = CombinePose.transform(vertex);
                        }

                        MeshData.Vertices.push_back(vertex);
                    }
                }

                for (uint32_t row = 0; row < NumRows - 1; row++) {
                    for (uint32_t col = 0; col < NumColumns - 1; col++) {
                        uint32_t idx00 = row * NumColumns + col;
                        uint32_t idx10 = idx00 + 1;
                        uint32_t idx01 = idx00 + NumColumns;
                        uint32_t idx11 = idx01 + 1;

                        MeshData.Indices.push_back(idx00);
                        MeshData.Indices.push_back(idx10);
                        MeshData.Indices.push_back(idx01);

                        MeshData.Indices.push_back(idx10);
                        MeshData.Indices.push_back(idx11);
                        MeshData.Indices.push_back(idx01);
                    }
                }

                MeshData.Flags = ShapeDataItem.m_shape_flags;
                MeshData.QueryFilterData = ShapeDataItem.Shape.ShapeCore.QueryFilterData;
                MeshData.SimulationFilterData = ShapeDataItem.Shape.ShapeCore.SimulationFilterData;
                MeshData.UniqueKey1 = UniqueKey;
                MeshData.UniqueKey2 = (uint64_t)((PxHeightFieldGeometryT&)ShapeDataItem.Shape.GetGeometry()).mHeightField;
                MeshData.Type = PxGeometryType::eHEIGHTFIELD;
                MeshData.Transform = CombinePose;
                MeshData.HashValue = MeshData.ComputeHash(); //
                //#pragma omp critical
                {
                    TriangleMeshDatas.push_back(MeshData);
                }
            }
			else if (ShapeDataItem.mType == PxGeometryType::eCONVEXMESH)
			{
				//PxConvexMeshGeometryT convex_geometry = (PxConvexMeshGeometryT&)ShapeDataItem.Shape.GetGeometry();
				const PxMeshScale ScaleVal = ShapeDataItem.Scale;
				const uint8_t nbVertices = ShapeDataItem.ConvexMesh.HullData.HullVerticesNb;
				const uint8_t nbPolygons = ShapeDataItem.ConvexMesh.HullData.PolygonsNb;
				TriangleMeshData MeshData{};
				MeshData.Vertices = ShapeDataItem.Vertices;
				const size_t vertexCount = MeshData.Vertices.size();
				for (size_t i = 0; i < vertexCount; i++) {
					//MeshData.Vertices[i] = ScaleVal.transform(MeshData.Vertices[i]);
					if (autoTransform) {
						MeshData.Vertices[i] = CombinePose.transform(ScaleVal.transform(MeshData.Vertices[i]));
					}
				}

				for (const auto& polygon : ShapeDataItem.polygons) {
					if (polygon.mNbVerts < 3) {
						continue;
					}

					if (polygon.mVRef8 >= ShapeDataItem.ConvexIndices.size()) {
						continue;
					}

					if (polygon.mVRef8 + polygon.mNbVerts > ShapeDataItem.ConvexIndices.size()) {
						continue;
					}

					for (uint16_t j = 0; j < polygon.mNbVerts - 2; j++) {
						if (polygon.mVRef8 + j + 2 >= ShapeDataItem.ConvexIndices.size()) {
							break;
						}

						uint32_t indices[3] = { ShapeDataItem.ConvexIndices[polygon.mVRef8], ShapeDataItem.ConvexIndices[polygon.mVRef8 + j + 1], ShapeDataItem.ConvexIndices[polygon.mVRef8 + j + 2] };

						bool valid = true;
						for (auto idx : indices) {
							if (idx >= ShapeDataItem.Vertices.size()) {
								valid = false;
								break;
							}
						}

						if (valid) {
							for (auto idx : indices) {
								MeshData.Indices.push_back(idx);
							}
						}
					}
				}

				MeshData.Flags = ShapeDataItem.m_shape_flags;
				MeshData.QueryFilterData = ShapeDataItem.Shape.ShapeCore.QueryFilterData;
				MeshData.SimulationFilterData = ShapeDataItem.Shape.ShapeCore.SimulationFilterData;
				MeshData.UniqueKey1 = UniqueKey;
				MeshData.Type = PxGeometryType::eCONVEXMESH;
				MeshData.Transform = CombinePose;
				MeshData.HashValue = MeshData.ComputeHash();
				//if (MeshData.QueryFilterData.word3 > 100) {
				{
					TriangleMeshDatas.push_back(MeshData);
				}
				// }
			}
		}
		return std::move(TriangleMeshDatas);
	}
}

#endif