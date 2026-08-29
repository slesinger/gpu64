/*
 gpu64 milestone 6, stage 14 -- the retained scene graph: nodes, transforms,
 and the active camera.

 Portable, like gpu64_3d_render.h and for the same reason: every transform bug
 -- ROTATE_LOCAL composing the wrong way, MOVE_LOCAL walking sideways instead
 of forward, a camera whose view is not actually the inverse of its pose -- is
 findable by tools/hostsim on a PC. What is deliberately NOT here is anything
 that resolves a mesh resource ID to a Gpu64_3dMesh: that table lives in
 gpu64_3d_class1.cpp because only core 0 can pull an upload off the C64 bus.
 DRAW_NODE therefore stays in gpu64_3d_class1.cpp too -- it is a resource
 lookup plus a call to the already-portable gpu64_3dDrawMesh(), not new
 rendering logic, exactly the way opDrawMesh() is a thin IO2 wrapper today.

 Wire formats and opcode semantics: project/milestone6_3d_design.md,
 "Transforms: no matrices cross the bus", and docs/class1-3d-mesh-reference.md's
 Scene nodes / Transforms tables. Not restated here.
*/
#ifndef _gpu64_3d_scene_h
#define _gpu64_3d_scene_h

#include <circle/types.h>
#include "gpu64_3d_math.h"
#include "gpu64_3d_render.h"		// Gpu64_3dState

// 256 object instances, per the design doc's own limit -- "the scene table,
// sized against the SCENE_COMMIT copy". There is no SCENE_COMMIT yet (stage
// 16), but the cap is part of the design, not of the loop, so it applies now.
#define GPU64_3D_MAX_NODES	256

#define GPU64_3D_NODE_NONE	0
#define GPU64_3D_NODE_OBJECT	1
#define GPU64_3D_NODE_CAMERA	2

struct Gpu64_3dNode
{
	u16	id;
	u8	type;
	u8	visible;		// SET_VISIBLE; nonzero = drawn

	u16	meshId;			// OBJECT only -- resolved by the caller

	Gpu64_3dVec	pos;		// 16.16 world space

	// Orientation is stored as the three absolute angles, not just the
	// matrix built from them. Two reasons: GET_TRANSFORM has to report
	// back exactly the yaw/pitch/roll a program set (or accumulated via
	// ROTATE_LOCAL), and there is no arcsin/atan2 in gpu64_3d_math.h to
	// recover angles from an arbitrary rotation matrix -- only the
	// forward direction, sin/cos-of-angle to matrix. ROTATE_LOCAL is
	// therefore plain wrapping addition on each angle independently, per
	// the design doc's own words for the wire format: "add a turn rate
	// and let it wrap: no clamp, no modulo". The matrix is a cache,
	// rebuilt whenever any of the three change.
	u16	yaw, pitch, roll;

	u16	scale;			// unsigned 8.8, uniform

	Gpu64_3dMat	rot;		// gpu64_3dMatFromEuler( yaw, pitch, roll )
};

struct Gpu64_3dScene
{
	Gpu64_3dNode	node[ GPU64_3D_MAX_NODES ];
	u16		activeCameraId;
	boolean		bHaveActiveCamera;
};

// Power-on / session-reset state: every node freed, no active camera. Mirrors
// gpu64_3dReset()'s treatment of the resource table -- a RUN/STOP+RESTORE
// must not leave the next program looking at a dead program's nodes.
void gpu64_3dSceneReset( Gpu64_3dScene *pScene );

// Finds a live node by ID. nType == GPU64_3D_NODE_NONE matches either type,
// same convention resFind() in gpu64_3d_class1.cpp uses for resources.
Gpu64_3dNode *gpu64_3dSceneFind( Gpu64_3dScene *pScene, u16 nId, u8 nType );

// --- lifecycle ($20-$24) -------------------------------------------------
// Every one of these returns a GPU64_3D_OK/GPU64_3D_BAD_ARGS/-style code from
// gpu64_3d_render.h's GPU64_3D_* set, repeated here so this file needs no
// firmware header -- same convention gpu64_3d_render.h's own mesh/texture
// builders use. GPU64_3D_OUT_OF_MEMORY here means the node table, not the
// resource arena.
#define GPU64_3D_BAD_ID		0x0A	// numerically identical to GPU64_ERR_BAD_ID

u8 gpu64_3dSceneCreateObject( Gpu64_3dScene *pScene, u16 nId, u16 nMeshId );
u8 gpu64_3dSceneCreateCamera( Gpu64_3dScene *pScene, u16 nId );
u8 gpu64_3dSceneDestroyNode( Gpu64_3dScene *pScene, u16 nId );
u8 gpu64_3dSceneSetActiveCamera( Gpu64_3dScene *pScene, u16 nId );
u8 gpu64_3dSceneSetVisible( Gpu64_3dScene *pScene, u16 nId, boolean bVisible );

// --- transforms ($30-$36) -------------------------------------------------
// All take world-space/absolute or local/delta quantities already widened to
// this file's internal units (16.16 positions, u16 binary angles) -- the
// caller (gpu64_3d_class1.cpp's opcode handlers) does the 8.8->16.16 and
// wire-byte unpacking, exactly as opDrawMesh() already does for DRAW_MESH's
// own position and rotation arguments.

u8 gpu64_3dSceneSetPosition( Gpu64_3dScene *pScene, u16 nId, const Gpu64_3dVec *pPos );
u8 gpu64_3dSceneSetOrientation( Gpu64_3dScene *pScene, u16 nId, u16 nYaw, u16 nPitch, u16 nRoll );

// dx/dy/dz already widened to 16.16 (the wire is 8.8; see above).
u8 gpu64_3dSceneMoveLocal( Gpu64_3dScene *pScene, u16 nId, s32 dx, s32 dy, s32 dz );
u8 gpu64_3dSceneMoveWorld( Gpu64_3dScene *pScene, u16 nId, s32 dx, s32 dy, s32 dz );

u8 gpu64_3dSceneRotateLocal( Gpu64_3dScene *pScene, u16 nId, u16 dYaw, u16 dPitch, u16 dRoll );
u8 gpu64_3dSceneSetScale( Gpu64_3dScene *pScene, u16 nId, u16 nScale );

u8 gpu64_3dSceneGetTransform( Gpu64_3dScene *pScene, u16 nId,
			       Gpu64_3dVec *pPos, u16 *pYaw, u16 *pPitch, u16 *pRoll );

// --- the active camera -----------------------------------------------------
// Derives pState->viewRot/viewPos from the active camera node's pose -- the
// view transform is the inverse of the camera's world transform: rotate by
// the transpose after subtracting the camera's position. Exactly what
// tools/hostsim/hostsim.cpp's own setCamera() has done by hand since before
// CREATE_CAMERA existed (see that file's "camera" section), which is what
// proved this is enough before any firmware node existed to drive it.
//
// With no active camera, sets the identity/zero pair -- draw in world space,
// same as DRAW_MESH always has. Deterministic either way: never leaves
// whatever was there before.
void gpu64_3dSceneApplyCamera( const Gpu64_3dScene *pScene, Gpu64_3dState *pState );

#endif
