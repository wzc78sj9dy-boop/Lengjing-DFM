//
// Created by binglen on 2025/6/8.
//

#ifndef PHYSX_ENUMS_H
#define PHYSX_ENUMS_H

#include <cstdint>

namespace PhysX
{
	enum class PxConcreteType : uint16_t
	{
		eUNDEFINED,
		eHEIGHTFIELD,
		eCONVEX_MESH,
		eTRIANGLE_MESH_BVH33,
		eTRIANGLE_MESH_BVH34,
		eCLOTH_FABRIC,
		eRIGID_DYNAMIC,
		eRIGID_STATIC,
		eSHAPE,
		eMATERIAL,
		eCONSTRAINT,
		eCLOTH,
		ePARTICLE_SYSTEM,
		ePARTICLE_FLUID,
		eAGGREGATE,
		eARTICULATION,
		eARTICULATION_LINK,
		eARTICULATION_JOINT,
		ePRUNING_STRUCTURE,
		ePHYSX_CORE_COUNT,
		eFIRST_PHYSX_EXTENSION = 256,
		eFIRST_VEHICLE_EXTENSION = 512,
		eFIRST_USER_EXTENSION = 1024
	};

	enum class PxBaseFlag : uint16_t
	{
		eOWNS_MEMORY = (1 << 0),
		eIS_RELEASABLE = (1 << 1)
	};

	enum class PxRigidBodyFlag : uint8_t
	{
		eKINEMATIC = (1 << 0),
		eUSE_KINEMATIC_TARGET_FOR_SCENE_QUERIES = (1 << 1),
		eENABLE_CCD = (1 << 2),
		eENABLE_CCD_FRICTION = (1 << 3),
		eENABLE_POSE_INTEGRATION_PREVIEW = (1 << 4),
		eENABLE_SPECULATIVE_CCD = (1 << 5),
		eENABLE_CCD_MAX_CONTACT_IMPULSE = (1 << 6)
	};

    enum class ActorType : uint16_t {
        DYNAMIC = 6,
        STATIC = 7,
        ARTICULATION_LINK = 11,  // 铰接体链接
        ARTICULATION = 12        // 铰接体
    };

//	enum class ActorType : uint16_t {
//		DYNAMIC = 6,
//		STATIC = 7
//	};
}

#endif