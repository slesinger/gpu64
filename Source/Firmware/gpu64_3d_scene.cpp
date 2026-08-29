/*
 gpu64 milestone 6, stage 14 -- the retained scene graph, as built.

 Portable: compiled unchanged by the firmware and by tools/hostsim. See
 gpu64_3d_scene.h for what is deliberately not here (mesh resolution).
*/
#include "gpu64_3d_scene.h"
#include <circle/util.h>

static void rebuildRot( Gpu64_3dNode *pN )
{
	gpu64_3dMatFromEuler( &pN->rot, pN->yaw, pN->pitch, pN->roll );
}

static void nodeDefaults( Gpu64_3dNode *pN, u16 nId, u8 nType )
{
	memset( pN, 0, sizeof( *pN ) );
	pN->id      = nId;
	pN->type    = nType;
	pN->visible = TRUE;			// a fresh node is drawn, not hidden --
						// same "visible over hidden" default the
						// colormap and the light use.
	pN->scale   = GPU64_FX8_ONE;		// 1.0
	gpu64_3dMatIdentity( &pN->rot );
}

void gpu64_3dSceneReset( Gpu64_3dScene *pScene )
{
	memset( pScene, 0, sizeof( *pScene ) );
}

Gpu64_3dNode *gpu64_3dSceneFind( Gpu64_3dScene *pScene, u16 nId, u8 nType )
{
	for ( unsigned i = 0; i < GPU64_3D_MAX_NODES; i++ )
		if ( pScene->node[ i ].type != GPU64_3D_NODE_NONE && pScene->node[ i ].id == nId )
			return ( nType == GPU64_3D_NODE_NONE || pScene->node[ i ].type == nType )
				? &pScene->node[ i ] : 0;
	return 0;
}

// Finds the slot for an ID, reusing it if the ID is already live -- same
// convention resSlot() in gpu64_3d_class1.cpp uses for resources: re-creating
// over a live ID replaces it rather than erroring, so a program that gets its
// own bookkeeping wrong at least gets a well-defined result instead of a
// second node nothing ever addresses again.
static Gpu64_3dNode *nodeSlot( Gpu64_3dScene *pScene, u16 nId )
{
	Gpu64_3dNode *pFree = 0;

	for ( unsigned i = 0; i < GPU64_3D_MAX_NODES; i++ )
	{
		if ( pScene->node[ i ].type != GPU64_3D_NODE_NONE && pScene->node[ i ].id == nId )
			return &pScene->node[ i ];
		if ( pScene->node[ i ].type == GPU64_3D_NODE_NONE && !pFree )
			pFree = &pScene->node[ i ];
	}

	return pFree;
}

// --- lifecycle ------------------------------------------------------------

u8 gpu64_3dSceneCreateObject( Gpu64_3dScene *pScene, u16 nId, u16 nMeshId )
{
	Gpu64_3dNode *pN = nodeSlot( pScene, nId );
	if ( pN == 0 )
		return GPU64_3D_OUT_OF_MEMORY;		// the node table, not the arena

	nodeDefaults( pN, nId, GPU64_3D_NODE_OBJECT );
	pN->meshId = nMeshId;
	return GPU64_3D_OK;
}

u8 gpu64_3dSceneCreateCamera( Gpu64_3dScene *pScene, u16 nId )
{
	Gpu64_3dNode *pN = nodeSlot( pScene, nId );
	if ( pN == 0 )
		return GPU64_3D_OUT_OF_MEMORY;

	nodeDefaults( pN, nId, GPU64_3D_NODE_CAMERA );
	return GPU64_3D_OK;
}

u8 gpu64_3dSceneDestroyNode( Gpu64_3dScene *pScene, u16 nId )
{
	Gpu64_3dNode *pN = gpu64_3dSceneFind( pScene, nId, GPU64_3D_NODE_NONE );
	if ( pN == 0 )
		return GPU64_3D_BAD_ID;

	// A destroyed active camera must not linger as a dangling ID: if the
	// slot is later reused by CREATE_OBJECT for an unrelated node,
	// ApplyCamera must not start treating it as a camera it was never
	// told about.
	if ( pScene->bHaveActiveCamera && pScene->activeCameraId == nId )
		pScene->bHaveActiveCamera = FALSE;

	pN->type = GPU64_3D_NODE_NONE;
	return GPU64_3D_OK;
}

u8 gpu64_3dSceneSetActiveCamera( Gpu64_3dScene *pScene, u16 nId )
{
	Gpu64_3dNode *pN = gpu64_3dSceneFind( pScene, nId, GPU64_3D_NODE_CAMERA );
	if ( pN == 0 )
		return GPU64_3D_BAD_ID;

	pScene->activeCameraId    = nId;
	pScene->bHaveActiveCamera = TRUE;
	return GPU64_3D_OK;
}

u8 gpu64_3dSceneSetVisible( Gpu64_3dScene *pScene, u16 nId, boolean bVisible )
{
	Gpu64_3dNode *pN = gpu64_3dSceneFind( pScene, nId, GPU64_3D_NODE_NONE );
	if ( pN == 0 )
		return GPU64_3D_BAD_ID;

	pN->visible = bVisible ? TRUE : FALSE;
	return GPU64_3D_OK;
}

// --- transforms -------------------------------------------------------------

u8 gpu64_3dSceneSetPosition( Gpu64_3dScene *pScene, u16 nId, const Gpu64_3dVec *pPos )
{
	Gpu64_3dNode *pN = gpu64_3dSceneFind( pScene, nId, GPU64_3D_NODE_NONE );
	if ( pN == 0 )
		return GPU64_3D_BAD_ID;

	pN->pos = *pPos;
	return GPU64_3D_OK;
}

u8 gpu64_3dSceneSetOrientation( Gpu64_3dScene *pScene, u16 nId, u16 nYaw, u16 nPitch, u16 nRoll )
{
	Gpu64_3dNode *pN = gpu64_3dSceneFind( pScene, nId, GPU64_3D_NODE_NONE );
	if ( pN == 0 )
		return GPU64_3D_BAD_ID;

	pN->yaw = nYaw; pN->pitch = nPitch; pN->roll = nRoll;
	rebuildRot( pN );
	return GPU64_3D_OK;
}

u8 gpu64_3dSceneMoveLocal( Gpu64_3dScene *pScene, u16 nId, s32 dx, s32 dy, s32 dz )
{
	Gpu64_3dNode *pN = gpu64_3dSceneFind( pScene, nId, GPU64_3D_NODE_NONE );
	if ( pN == 0 )
		return GPU64_3D_BAD_ID;

	// "Forward 1.5" has to mean forward for *this* node, whatever it is
	// currently facing -- the delta is in the node's own axes, so it is
	// rotated by the node's current orientation before being added.
	Gpu64_3dVec delta, rotated;
	delta.x = dx; delta.y = dy; delta.z = dz;
	gpu64_3dVecRotate( &rotated, &pN->rot, &delta );

	pN->pos.x += rotated.x;
	pN->pos.y += rotated.y;
	pN->pos.z += rotated.z;
	return GPU64_3D_OK;
}

u8 gpu64_3dSceneMoveWorld( Gpu64_3dScene *pScene, u16 nId, s32 dx, s32 dy, s32 dz )
{
	Gpu64_3dNode *pN = gpu64_3dSceneFind( pScene, nId, GPU64_3D_NODE_NONE );
	if ( pN == 0 )
		return GPU64_3D_BAD_ID;

	pN->pos.x += dx;
	pN->pos.y += dy;
	pN->pos.z += dz;
	return GPU64_3D_OK;
}

u8 gpu64_3dSceneRotateLocal( Gpu64_3dScene *pScene, u16 nId, u16 dYaw, u16 dPitch, u16 dRoll )
{
	Gpu64_3dNode *pN = gpu64_3dSceneFind( pScene, nId, GPU64_3D_NODE_NONE );
	if ( pN == 0 )
		return GPU64_3D_BAD_ID;

	// Plain wrapping addition on each angle independently -- see the "why"
	// in gpu64_3d_scene.h. u16 arithmetic wraps mod 65536 for free, which
	// is exactly the design doc's "add a turn rate and let it wrap".
	pN->yaw   = (u16)( pN->yaw   + dYaw );
	pN->pitch = (u16)( pN->pitch + dPitch );
	pN->roll  = (u16)( pN->roll  + dRoll );
	rebuildRot( pN );
	return GPU64_3D_OK;
}

u8 gpu64_3dSceneSetScale( Gpu64_3dScene *pScene, u16 nId, u16 nScale )
{
	if ( nScale == 0 )
		return GPU64_3D_BAD_ARGS;	// same rejection DRAW_MESH already
						// makes at draw time; catching it
						// here fails at the opcode that
						// caused it, not three calls later.

	Gpu64_3dNode *pN = gpu64_3dSceneFind( pScene, nId, GPU64_3D_NODE_NONE );
	if ( pN == 0 )
		return GPU64_3D_BAD_ID;

	pN->scale = nScale;
	return GPU64_3D_OK;
}

u8 gpu64_3dSceneGetTransform( Gpu64_3dScene *pScene, u16 nId,
			       Gpu64_3dVec *pPos, u16 *pYaw, u16 *pPitch, u16 *pRoll )
{
	Gpu64_3dNode *pN = gpu64_3dSceneFind( pScene, nId, GPU64_3D_NODE_NONE );
	if ( pN == 0 )
		return GPU64_3D_BAD_ID;

	*pPos = pN->pos;
	*pYaw = pN->yaw; *pPitch = pN->pitch; *pRoll = pN->roll;
	return GPU64_3D_OK;
}

// --- the active camera ------------------------------------------------------

void gpu64_3dSceneApplyCamera( const Gpu64_3dScene *pScene, Gpu64_3dState *pState )
{
	const Gpu64_3dNode *pCam = 0;
	if ( pScene->bHaveActiveCamera )
	{
		// Not gpu64_3dSceneFind(): that takes a non-const scene, and this
		// path must not mutate one. The scan is the same either way.
		for ( unsigned i = 0; i < GPU64_3D_MAX_NODES; i++ )
			if ( pScene->node[ i ].type == GPU64_3D_NODE_CAMERA &&
			     pScene->node[ i ].id == pScene->activeCameraId )
			{
				pCam = &pScene->node[ i ];
				break;
			}
	}

	if ( pCam == 0 )
	{
		// No active camera: draw in world space, exactly as DRAW_MESH
		// always has. Deterministic -- never leaves whatever the state
		// carried from a previous call.
		gpu64_3dMatIdentity( &pState->viewRot );
		pState->viewPos.x = pState->viewPos.y = pState->viewPos.z = 0;
		pState->bHaveCamera = FALSE;
		return;
	}

	// View = rotate by the inverse of the camera's rotation (its
	// transpose, since it is a pure rotation), after subtracting the
	// camera's position. tools/hostsim/hostsim.cpp's setCamera() has done
	// exactly this by hand since before CREATE_CAMERA existed in
	// firmware, which is what proved it was enough.
	gpu64_3dMatTranspose( &pState->viewRot, &pCam->rot );
	pState->viewPos = pCam->pos;
	pState->bHaveCamera = TRUE;
}
