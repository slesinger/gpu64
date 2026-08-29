/*
 gpu64 milestone 6 -- class 1 on core 0: session state, the resource table,
 and the opcode dispatcher.

 Why core 0, when the design's architecture section puts the renderer on core
 1: because immediate mode is the bring-up path, and immediate mode has no
 cross-core state by construction. DRAW_MESH is specified to return when the
 mesh is drawn and to be illegal while the render loop runs -- so there is
 exactly one owner of the framebuffer and the z-buffer at any instant, and
 running it here makes that owner core 0, inside the dispatch window where the
 C64 is already DMA-halted. That is precisely the model every class 0 draw op
 already uses, and it means the entire pipeline can be brought up on hardware
 with the cross-core question still open.

 The ring is still fed -- every accepted class 1 command is pushed to it and
 counted by core 1 (gpu64_3d_core1.cpp) -- so the cross-core path stays under
 real traffic while that question is settled.

 Nothing here is portable; the pipeline it drives (gpu64_3d_render.h) is, and
 is exercised on a PC by tools/hostsim.
*/
#include "gpu64_3d_internals.h"
#include "gpu64_3d_render.h"
#include "gpu64_3d_scene.h"
#include "gpu64_api.h"
#include "gpu64_fb.h"
#include "lowlevel_arm64.h"
#include <circle/util.h>

#ifdef GPU64_3D_ENABLED

// --- the arena ----------------------------------------------------------

static u8  s_Arena[ GPU64_3D_ARENA_BYTES ] __attribute__(( aligned( 64 ) ));
static u32 s_ArenaUsed;

void *gpu64_3dArenaAlloc( u32 nBytes )
{
	// Every allocation starts on a cache line. Wasteful at the byte level and
	// worth it: a texture whose rows share a line with the end of a mesh is a
	// false-sharing bug waiting for the frame core 1 and core 0 happen to
	// touch both in.
	nBytes = ( nBytes + 63 ) & ~63u;

	if ( nBytes > GPU64_3D_ARENA_BYTES - s_ArenaUsed )
		return 0;					// caller answers OUT_OF_MEMORY

	void *p = s_Arena + s_ArenaUsed;
	s_ArenaUsed += nBytes;
	return p;
}

void gpu64_3dArenaReset( void )
{
	// Bump allocator: reset is the whole free path. Nothing is zeroed --
	// every allocation is written before it is read, and zeroing 32 MB here
	// would be the longest unbroken store burst in the firmware.
	s_ArenaUsed = 0;
}

u32 gpu64_3dArenaUsed( void )
{
	return s_ArenaUsed;
}

static void *arenaAllocFn( void *, u32 nBytes )
{
	return gpu64_3dArenaAlloc( nBytes );
}

// --- session state ------------------------------------------------------

static Gpu64_3dState	s_State;
static Gpu64_3dResource	s_Res[ GPU64_3D_MAX_RESOURCES ];
static Gpu64_3dScratch	s_Scratch;

// The scene graph -- stage 14. Nodes reference meshes by ID; resolving that
// ID to a Gpu64_3dResource stays here rather than in gpu64_3d_scene.cpp, for
// the reason given at the top of gpu64_3d_scene.h.
static Gpu64_3dScene	s_Scene;

// The z-buffer, sized for the largest viewport SET_VIEWPORT will accept.
// Static rather than out of the arena so a program cannot exhaust resource
// RAM and then find it cannot draw.
static u16 s_Depth[ GPU64_3D_SURFACE_W * GPU64_3D_SURFACE_H ] __attribute__(( aligned( 64 ) ));

// Blob staging. An upload is pulled off the bus into one of these and then
// parsed into the arena -- the parse is what turns the wire format into the
// form the rasteriser wants (unpacked faces, precomputed normals), so a
// staging step exists whether or not it is spelled out.
static u8 s_StageA[ 65536 ];
static u8 s_StageB[ 65536 ];

#define sArg	gpu64Regs.arg

static inline u16 argU16( unsigned i )
{
	return (u16)( sArg[ i ] | ( sArg[ i + 1 ] << 8 ) );
}

static inline s16 argS16( unsigned i )
{
	return (s16)argU16( i );
}

static inline s32 argS32( unsigned i )
{
	return (s32)( (u32)sArg[ i ] | ( (u32)sArg[ i + 1 ] << 8 ) |
		       ( (u32)sArg[ i + 2 ] << 16 ) | ( (u32)sArg[ i + 3 ] << 24 ) );
}

static inline void argBlob( unsigned i, u8 *pSpace, u32 *pAddr, u32 *pLen )
{
	*pSpace = sArg[ i ];
	*pAddr  = (u32)sArg[ i + 1 ] | ( (u32)sArg[ i + 2 ] << 8 ) | ( (u32)sArg[ i + 3 ] << 16 );
	*pLen   = (u32)sArg[ i + 4 ] | ( (u32)sArg[ i + 5 ] << 8 );
}

static inline u16 stagedId( void )
{
	return (u16)( gpu64Regs.id[ 0 ] | ( gpu64Regs.id[ 1 ] << 8 ) );
}

// --- the resource table -------------------------------------------------

static Gpu64_3dResource *resFind( u16 nId, u8 nType )
{
	for ( unsigned i = 0; i < GPU64_3D_MAX_RESOURCES; i++ )
		if ( s_Res[ i ].type != GPU64_3D_RES_NONE && s_Res[ i ].id == nId )
			return ( nType == GPU64_3D_RES_NONE || s_Res[ i ].type == nType )
				? &s_Res[ i ] : 0;
	return 0;
}

// Finds the slot for an ID, reusing it if the ID is already live. Re-upload
// to a live ID replaces it, per the design's Resource lifecycle.
//
// The bytes of the old allocation are NOT reclaimed: the arena is a bump
// allocator, so a replaced resource leaks until the session resets. That is a
// deliberate phase-1 limit, not an oversight -- it keeps the allocator out of
// a path core 1 will one day share, and 32 MB absorbs a great many reloads.
// Anything that re-uploads in a loop will exhaust it and get OUT_OF_MEMORY,
// which is at least a truthful error.
static Gpu64_3dResource *resSlot( u16 nId )
{
	Gpu64_3dResource *pFree = 0;

	for ( unsigned i = 0; i < GPU64_3D_MAX_RESOURCES; i++ )
	{
		if ( s_Res[ i ].type != GPU64_3D_RES_NONE && s_Res[ i ].id == nId )
			return &s_Res[ i ];
		if ( s_Res[ i ].type == GPU64_3D_RES_NONE && !pFree )
			pFree = &s_Res[ i ];
	}

	return pFree;
}

static const Gpu64_3dTexture *lookupTexture( void *, u16 nId )
{
	const Gpu64_3dResource *pR = resFind( nId, GPU64_3D_RES_TEXTURE );
	// A face naming a texture that was never uploaded falls back to flat
	// colour rather than failing the draw: one missing texture should not
	// blank a scene, and the wrong-looking face is a better diagnostic than
	// an error code the C64 sees after the fact.
	return pR ? &pR->tex : 0;
}

// --- lifecycle ----------------------------------------------------------

void gpu64_3dInit( void )
{
	memset( &gpu64_3dRing, 0, sizeof( gpu64_3dRing ) );
	memset( (void *)&gpu64_3dWorkerStats, 0, sizeof( gpu64_3dWorkerStats ) );
	memset( &gpu64_3dHost, 0, sizeof( gpu64_3dHost ) );
	memset( s_Res, 0, sizeof( s_Res ) );
	gpu64_3dArenaReset();
	gpu64_3dStateDefaults( &s_State );
	gpu64_3dSceneReset( &s_Scene );
}

void gpu64_3dReset( void )
{
	// A session reset has to leave the ring empty, and only core 0 can say
	// so: core 1 is mid-drain and would happily execute commands belonging to
	// the program that just died. Moving the tail forward from core 0 is the
	// one place that rule is broken, and it is safe precisely because the C64
	// is halted and no new command can arrive while it happens.
	gpu64_3dRing.tail = gpu64_3dRing.head;
	gpu64_3dRing.tailCache = gpu64_3dRing.head;

	// Every resource of the session goes with it -- design doc, Resource
	// lifecycle. Without this a RUN/STOP+RESTORE leaks the whole arena and
	// the next program starts against stale IDs.
	memset( s_Res, 0, sizeof( s_Res ) );
	gpu64_3dArenaReset();
	gpu64_3dStateDefaults( &s_State );

	// Every node of the session goes with it too, same rule and same
	// reason as the resource table above: RUN/STOP+RESTORE must not leave
	// the next program looking at a dead program's scene.
	gpu64_3dSceneReset( &s_Scene );

	memset( &gpu64_3dHost, 0, sizeof( gpu64_3dHost ) );
}

// --- the draw target ----------------------------------------------------

static boolean makeTarget( Gpu64_3dTarget *pTarget )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	if ( pFB == 0 || !pFB->IsInitialized() )
		return FALSE;

	pTarget->pPixels = pFB->PageBuffer( pFB->GetDrawPage() );
	pTarget->pitch   = pFB->GetPitch();
	pTarget->pDepth  = s_Depth;
	return pTarget->pPixels != 0;
}

// The VideoCore scans DRAM directly and is not coherent with the ARM, so
// nothing drawn is visible until the rows are cleaned -- same rule every
// class 0 draw op follows. Only the viewport is cleaned: the rest of the page
// belongs to the C64's HUD and was cleaned when the C64 drew it.
static void cleanViewport( void )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	if ( pFB )
		pFB->CleanRows( pFB->GetDrawPage(), s_State.vpY, s_State.vpY + s_State.vpH );
}

// --- opcodes ------------------------------------------------------------

static u8 opSetViewport( void )
{
	const u16 x = argU16( 0 ), y = argU16( 2 ), w = argU16( 4 ), h = argU16( 6 );

	if ( w == 0 || h == 0 )
		return GPU64_ERR_BAD_ARGS;
	if ( (u32)x + w > GPU64_3D_SURFACE_W || (u32)y + h > GPU64_3D_SURFACE_H )
		return GPU64_ERR_BAD_ARGS;
	if ( (u32)w * h * 3 > GPU64_3D_BUDGET )
		return GPU64_ERR_OUT_OF_RANGE;

	s_State.vpX = x;
	s_State.vpY = y;
	s_State.vpW = w;
	s_State.vpH = h;

	// The focal length is derived from the fov and the viewport width, so a
	// viewport change re-derives it. Not doing so is a subtle one: the scene
	// keeps rendering and quietly has the wrong field of view.
	s_State.focal = gpu64_3dFocalFromFov( s_State.fov, w );
	return GPU64_ERR_OK;
}

static u8 opSetPerspective( void )
{
	const u16 fov  = argU16( 0 );
	const s32 near = (s32)(s16)argU16( 2 ) << 8;		// 8.8 -> 16.16
	const s32 far  = (s32)(s16)argU16( 4 ) << 8;

	if ( near <= 0 || far <= near )
		return GPU64_ERR_BAD_ARGS;

	const s32 focal = gpu64_3dFocalFromFov( fov, s_State.vpW );
	if ( focal <= 0 )
		return GPU64_ERR_BAD_ARGS;

	s_State.fov   = fov;
	s_State.focal = focal;
	s_State.nearZ = near;
	s_State.farZ  = far;
	return GPU64_ERR_OK;
}

static u8 opSetLight( void )
{
	if ( sArg[ 6 ] > 15 )
		return GPU64_ERR_BAD_ARGS;

	if ( !gpu64_3dNormalise( s_State.lightDir, argS16( 0 ), argS16( 2 ), argS16( 4 ) ) )
		return GPU64_ERR_BAD_ARGS;		// a zero-length direction

	s_State.ambient = sArg[ 6 ];
	return GPU64_ERR_OK;
}

static u8 opBuildColormap( void )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	if ( pFB == 0 )
		return GPU64_ERR_UNSUPPORTED;

	gpu64_3dBuildColormap( &s_State, pFB->GetPaletteRGB() );
	return GPU64_ERR_OK;
}

static u8 opUploadMesh( void )
{
	u8 spaceV, spaceF;
	u32 addrV, lenV, addrF, lenF;

	argBlob( 0, &spaceV, &addrV, &lenV );
	argBlob( 6, &spaceF, &addrF, &lenF );

	if ( lenV == 0 || lenF == 0 || lenV > sizeof( s_StageA ) || lenF > sizeof( s_StageB ) )
		return GPU64_ERR_BAD_ARGS;

	u8 res = gpu64_blobRead( spaceV, addrV, lenV, s_StageA );
	if ( res != GPU64_ERR_OK ) return res;

	res = gpu64_blobRead( spaceF, addrF, lenF, s_StageB );
	if ( res != GPU64_ERR_OK ) return res;

	Gpu64_3dResource *pR = resSlot( stagedId() );
	if ( pR == 0 )
		return GPU64_ERR_OUT_OF_MEMORY;		// the table, not the arena

	Gpu64_3dMesh mesh;
	res = gpu64_3dBuildMesh( &mesh, s_StageA, lenV, s_StageB, lenF, arenaAllocFn, 0 );
	if ( res != GPU64_3D_OK )
		return res;				// the slot is untouched on failure

	pR->id   = stagedId();
	pR->type = GPU64_3D_RES_MESH;
	pR->mesh = mesh;

	gpu64Regs.result = (u8)mesh.nFaces;
	return GPU64_ERR_OK;
}

static u8 opUploadTexture( void )
{
	u8 space;
	u32 addr, len;
	argBlob( 0, &space, &addr, &len );

	if ( len == 0 || len > sizeof( s_StageA ) )
		return GPU64_ERR_BAD_ARGS;

	u8 res = gpu64_blobRead( space, addr, len, s_StageA );
	if ( res != GPU64_ERR_OK ) return res;

	Gpu64_3dResource *pR = resSlot( stagedId() );
	if ( pR == 0 )
		return GPU64_ERR_OUT_OF_MEMORY;

	Gpu64_3dTexture tex;
	res = gpu64_3dBuildTexture( &tex, s_StageA, len, sArg[ 6 ], sArg[ 7 ], arenaAllocFn, 0 );
	if ( res != GPU64_3D_OK )
		return res;

	pR->id   = stagedId();
	pR->type = GPU64_3D_RES_TEXTURE;
	pR->tex  = tex;
	return GPU64_ERR_OK;
}

static u8 opFreeResource( void )
{
	Gpu64_3dResource *pR = resFind( stagedId(), GPU64_3D_RES_NONE );
	if ( pR == 0 )
		return GPU64_ERR_BAD_ID;

	// Table slot only -- see resSlot() on why the bytes stay allocated.
	pR->type = GPU64_3D_RES_NONE;
	return GPU64_ERR_OK;
}

static u8 opClearViewport( void )
{
	Gpu64_3dTarget target;
	if ( !makeTarget( &target ) )
		return GPU64_ERR_UNSUPPORTED;

	gpu64_3dClearViewport( &s_State, &target );
	cleanViewport();
	return GPU64_ERR_OK;
}

static u8 opDrawMesh( void )
{
	const Gpu64_3dResource *pR = resFind( stagedId(), GPU64_3D_RES_MESH );
	if ( pR == 0 )
		return GPU64_ERR_BAD_ID;

	Gpu64_3dTarget target;
	if ( !makeTarget( &target ) )
		return GPU64_ERR_UNSUPPORTED;

	Gpu64_3dVec pos;
	pos.x = (s32)argS16( 0 ) << 8;			// 8.8 -> 16.16
	pos.y = (s32)argS16( 2 ) << 8;
	pos.z = (s32)argS16( 4 ) << 8;

	Gpu64_3dMat rot;
	gpu64_3dMatFromEuler( &rot, argU16( 6 ), argU16( 8 ), argU16( 10 ) );

	u16 scale = argU16( 12 );
	if ( scale == 0 )
		return GPU64_ERR_BAD_ARGS;

	const unsigned n = gpu64_3dDrawMesh( &s_State, &target, &s_Scratch, &pR->mesh,
					     &pos, &rot, scale, lookupTexture, 0 );
	cleanViewport();

	// RESULT is the triangle count, saturated to a byte. It is the difference
	// between "the mesh was drawn and you are looking at the wrong part of
	// the screen" and "every face was culled", which a blank viewport cannot
	// tell you -- and on this hardware that distinction is otherwise a trip
	// to the bench.
	gpu64Regs.result = (u8)( n > 255 ? 255 : n );
	return GPU64_ERR_OK;
}

// --- the scene graph ($20-$24, $30-$36, $42) -----------------------------
//
// Thin IO2 wrappers, same shape throughout: unpack ARG into the units
// gpu64_3d_scene.h wants, call the portable function, translate its
// GPU64_3D_* code to the GPU64_ERR_* one the C64 side already knows (they
// share numeric values for BAD_ID/BAD_ARGS/OUT_OF_MEMORY/OK by construction,
// so the "translation" is the identity -- spelled out anyway so a future
// divergence between the two enums fails to compile instead of miscoding).

static u8 opCreateObject( void )
{
	return gpu64_3dSceneCreateObject( &s_Scene, stagedId(), argU16( 0 ) );
}

static u8 opCreateCamera( void )
{
	return gpu64_3dSceneCreateCamera( &s_Scene, stagedId() );
}

static u8 opDestroyNode( void )
{
	return gpu64_3dSceneDestroyNode( &s_Scene, stagedId() );
}

static u8 opSetActiveCamera( void )
{
	return gpu64_3dSceneSetActiveCamera( &s_Scene, stagedId() );
}

static u8 opSetVisible( void )
{
	if ( sArg[ 0 ] > 1 )
		return GPU64_ERR_BAD_ARGS;
	return gpu64_3dSceneSetVisible( &s_Scene, stagedId(), sArg[ 0 ] != 0 );
}

static u8 opSetPosition( void )
{
	Gpu64_3dVec pos;
	pos.x = argS32( 0 );		// wire is already 16.16 -- SET_POSITION places
	pos.y = argS32( 4 );		// a node anywhere in a 65536-unit world, unlike
	pos.z = argS32( 8 );		// DRAW_MESH's 8.8 immediate-mode offset.
	return gpu64_3dSceneSetPosition( &s_Scene, stagedId(), &pos );
}

static u8 opSetOrientation( void )
{
	return gpu64_3dSceneSetOrientation( &s_Scene, stagedId(),
					     argU16( 0 ), argU16( 2 ), argU16( 4 ) );
}

static u8 opMoveLocal( void )
{
	return gpu64_3dSceneMoveLocal( &s_Scene, stagedId(),
					(s32)argS16( 0 ) << 8,		// 8.8 -> 16.16
					(s32)argS16( 2 ) << 8,
					(s32)argS16( 4 ) << 8 );
}

static u8 opMoveWorld( void )
{
	return gpu64_3dSceneMoveWorld( &s_Scene, stagedId(),
					(s32)argS16( 0 ) << 8,
					(s32)argS16( 2 ) << 8,
					(s32)argS16( 4 ) << 8 );
}

static u8 opRotateLocal( void )
{
	return gpu64_3dSceneRotateLocal( &s_Scene, stagedId(),
					  argU16( 0 ), argU16( 2 ), argU16( 4 ) );
}

static u8 opSetScale( void )
{
	return gpu64_3dSceneSetScale( &s_Scene, stagedId(), argU16( 0 ) );
}

static u8 opGetTransform( void )
{
	u8  space;
	u32 addr, len;
	argBlob( 0, &space, &addr, &len );
	if ( len < 18 )
		return GPU64_ERR_BAD_ARGS;

	Gpu64_3dVec pos;
	u16 yaw, pitch, roll;
	const u8 res = gpu64_3dSceneGetTransform( &s_Scene, stagedId(), &pos, &yaw, &pitch, &roll );
	if ( res != GPU64_ERR_OK )
		return res;

	// Position (s32 16.16 x/y/z) then yaw/pitch/roll (u16 each), little-endian
	// throughout -- the same byte order every other multi-byte field on this
	// bus uses. 18 bytes, matching the wire format's own accounting.
	u8 buf[ 18 ];
	buf[ 0 ] = (u8)( pos.x );        buf[ 1 ] = (u8)( pos.x >> 8 );
	buf[ 2 ] = (u8)( pos.x >> 16 );  buf[ 3 ] = (u8)( pos.x >> 24 );
	buf[ 4 ] = (u8)( pos.y );        buf[ 5 ] = (u8)( pos.y >> 8 );
	buf[ 6 ] = (u8)( pos.y >> 16 );  buf[ 7 ] = (u8)( pos.y >> 24 );
	buf[ 8 ] = (u8)( pos.z );        buf[ 9 ] = (u8)( pos.z >> 8 );
	buf[ 10 ] = (u8)( pos.z >> 16 ); buf[ 11 ] = (u8)( pos.z >> 24 );
	buf[ 12 ] = (u8)( yaw );         buf[ 13 ] = (u8)( yaw >> 8 );
	buf[ 14 ] = (u8)( pitch );       buf[ 15 ] = (u8)( pitch >> 8 );
	buf[ 16 ] = (u8)( roll );        buf[ 17 ] = (u8)( roll >> 8 );

	return gpu64_blobWrite( space, addr, 18, buf );
}

static u8 opDrawNode( void )
{
	// gpu64: self-warm, rule 4 in CLAUDE.md -- "cycle-critical code reachable
	// from a command must warm its own i-cache at the point of use." Unlike
	// DRAW_MESH's path (opDrawMesh -> gpu64_3dDrawMesh -> the rasteriser),
	// which has been dispatched, and therefore self-warmed by execution,
	// continuously since milestone 6, this function and the two scene-graph
	// calls below it are new in Stage 14 and on a cold boot are guaranteed
	// never to have executed before their first DRAW_NODE. warmCache() at
	// boot (rad_main.cpp) does not cover them -- it only preloads the
	// polling loop and the other functions the loop calls directly -- and
	// gpu64_apiWarmPollingLoop() (gpu64_3dDispatch(), below) re-warms only
	// after a dispatch, too late to help this one. Same in-dispatch idiom as
	// gpu64_blobRead()/gpu64_blobWrite() in rad_reu.cpp -- acc == size, one
	// linear pass, not warmCache()'s boot-time acc=65536 (that one pays for
	// itself once at start-up; this one runs every dispatch with the bus
	// held, so it should cost exactly what it needs to and no more).
	// gpu64_3dDrawMesh() itself is deliberately not repeated here -- it is
	// already covered by DRAW_MESH's own history of use.
	//
	// This was not, on its own, what actually caused the Stage-14 hardware
	// regression -- that turned out to be reuUsingPolling() itself landing
	// on an execute-never MMU page once DRAW_NODE's code pushed the image
	// past a 64KB boundary (fixed at each ".text.section_polling" site in
	// rad_reu.cpp, 2026-08-29). It stays because rule 4 is a real, separate
	// concern this codebase has been bitten by more than once, and this path
	// is new enough on hardware to be worth not gambling on.
	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)opDrawNode, 1024 * 2 );
	FORCE_READ_LINEARa( (void*)opDrawNode, 1024 * 2, 1024 * 2 );
	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)gpu64_3dSceneFind, 1024 * 2 );
	FORCE_READ_LINEARa( (void*)gpu64_3dSceneFind, 1024 * 2, 1024 * 2 );
	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)gpu64_3dSceneApplyCamera, 1024 * 2 );
	FORCE_READ_LINEARa( (void*)gpu64_3dSceneApplyCamera, 1024 * 2, 1024 * 2 );

	Gpu64_3dNode *pN = gpu64_3dSceneFind( &s_Scene, stagedId(), GPU64_3D_NODE_OBJECT );
	if ( pN == 0 )
		return GPU64_ERR_BAD_ID;

	if ( !pN->visible )
	{
		// Not an error -- SET_VISIBLE(0) is how a program parks a node
		// without destroying it, exactly like DRAW_MESH's own "every face
		// culled" result below.
		gpu64Regs.result = 0;
		return GPU64_ERR_OK;
	}

	const Gpu64_3dResource *pR = resFind( pN->meshId, GPU64_3D_RES_MESH );
	if ( pR == 0 )
		return GPU64_ERR_BAD_ID;

	Gpu64_3dTarget target;
	if ( !makeTarget( &target ) )
		return GPU64_ERR_UNSUPPORTED;

	// The active camera, if any, is applied fresh on every DRAW_NODE: a
	// program that moves the camera between two DRAW_NODE calls in the same
	// frame must see both nodes drawn from where the camera is *now*, not
	// from wherever it was at some earlier SET_ACTIVE_CAMERA.
	gpu64_3dSceneApplyCamera( &s_Scene, &s_State );

	const unsigned n = gpu64_3dDrawMesh( &s_State, &target, &s_Scratch, &pR->mesh,
					     &pN->pos, &pN->rot, pN->scale, lookupTexture, 0 );
	cleanViewport();

	gpu64Regs.result = (u8)( n > 255 ? 255 : n );
	return GPU64_ERR_OK;
}

// --- dispatch -----------------------------------------------------------

static u8 execute( u8 op )
{
	switch ( op )
	{
	case GPU64_3D_OP_SCENE_RESET:
		// "Destroys every node, stops the loop, leaves uploaded resources
		// alone" -- docs/class1-3d-mesh-reference.md. The loop is phase 4
		// and does not exist yet; nodes and render state do.
		gpu64_3dStateDefaults( &s_State );
		gpu64_3dSceneReset( &s_Scene );
		return GPU64_ERR_OK;

	case GPU64_3D_OP_SET_VIEWPORT:		return opSetViewport();
	case GPU64_3D_OP_SET_PERSPECTIVE:	return opSetPerspective();
	case GPU64_3D_OP_SET_LIGHT:		return opSetLight();
	case GPU64_3D_OP_BUILD_COLORMAP:	return opBuildColormap();

	case GPU64_3D_OP_SET_BACKGROUND:
		s_State.background = sArg[ 0 ];
		return GPU64_ERR_OK;

	case GPU64_3D_OP_ARENA_STATUS:
	{
		// Free arena in 128 KB units. A 32 MB arena is exactly 256 of them,
		// so a full arena reads 255 (saturated) and an exhausted one reads 0
		// -- the whole range is usable and no scaling constant has to be
		// agreed with the C64 side beyond "128 KB".
		const u32 free = GPU64_3D_ARENA_BYTES - gpu64_3dArenaUsed();
		const u32 units = free >> 17;
		gpu64Regs.result = (u8)( units > 255 ? 255 : units );
		return GPU64_ERR_OK;
	}

	case GPU64_3D_OP_LOOP_STOP:
		// No loop runs yet, so stopping one always succeeds. Answering
		// BAD_OPCODE instead would make a correctly-written program fail at
		// its teardown.
		return GPU64_ERR_OK;

	case GPU64_3D_OP_UPLOAD_MESH:		return opUploadMesh();
	case GPU64_3D_OP_UPLOAD_TEXTURE:	return opUploadTexture();
	case GPU64_3D_OP_FREE_RESOURCE:		return opFreeResource();

	case GPU64_3D_OP_CREATE_OBJECT:		return opCreateObject();
	case GPU64_3D_OP_CREATE_CAMERA:		return opCreateCamera();
	case GPU64_3D_OP_DESTROY_NODE:		return opDestroyNode();
	case GPU64_3D_OP_SET_ACTIVE_CAMERA:	return opSetActiveCamera();
	case GPU64_3D_OP_SET_VISIBLE:		return opSetVisible();

	case GPU64_3D_OP_SET_POSITION:		return opSetPosition();
	case GPU64_3D_OP_SET_ORIENTATION:	return opSetOrientation();
	case GPU64_3D_OP_MOVE_LOCAL:		return opMoveLocal();
	case GPU64_3D_OP_MOVE_WORLD:		return opMoveWorld();
	case GPU64_3D_OP_ROTATE_LOCAL:		return opRotateLocal();
	case GPU64_3D_OP_SET_SCALE:		return opSetScale();
	case GPU64_3D_OP_GET_TRANSFORM:		return opGetTransform();

	case GPU64_3D_OP_CLEAR_VIEWPORT:	return opClearViewport();
	case GPU64_3D_OP_DRAW_MESH:		return opDrawMesh();
	case GPU64_3D_OP_DRAW_NODE:		return opDrawNode();

	case GPU64_3D_OP_LOOP_START:
	case GPU64_3D_OP_SCENE_COMMIT:
		// Both belong to the autonomous loop, which is phase 4. UNSUPPORTED
		// rather than BAD_OPCODE: the opcode is real and this build cannot do
		// it, which is a different thing for a program to branch on than an
		// opcode that does not exist.
		return GPU64_ERR_UNSUPPORTED;
	}

	return GPU64_ERR_BAD_OPCODE;
}

u8 gpu64_3dDispatch( u8 op )
{
	// The ring is fed first, and a full ring refuses the command outright:
	// the design's rule is that a failed dispatch does nothing, so a command
	// must not execute and then be reported as rejected.
	if ( !gpu64_3dRingPush( op ) )
	{
		gpu64_3dHost.rejected++;
		return GPU64_ERR_QUEUE_FULL;
	}

	const u8 res = execute( op );

	// gpu64: put back what this dispatch just evicted, while the bus is still
	// held. A DRAW_MESH walks a framebuffer, a z-buffer and the arena, which
	// is far more than the class 0 CLEAR that was already enough to evict
	// warmCache()'s work -- rule 4 in project/progress_tracker.md's polling-loop
	// timing rules, "preloaded at start-up is not durable". Doing it here
	// rather than in the loop is rule 5, and it is what makes the cost free:
	// it lands inside the window the C64 is already stopped for.
	//
	// Unconditional rather than only after the heavy opcodes: a dispatch is
	// the biggest instruction-cache consumer in the system whatever it does,
	// and the warm is two data preloads and one window preload against a
	// dispatch that has already paid for a DMA round trip.
	gpu64_apiWarmPollingLoop();

	if ( res == GPU64_ERR_BAD_OPCODE )
		gpu64_3dHost.badOpcode++;
	else
		gpu64_3dHost.pushed++;

	return res;
}

char *gpu64_3dReport( char *p )
{
	static const char hex[] = "0123456789ABCDEF";

	// Deliberately terse: this goes into the HDMI log overlay, which is 40
	// columns.
	const char *pLabel = "3D ok/rej/bad ";
	while ( *pLabel )
		*p++ = *pLabel++;

	u32 v[ 3 ] = { gpu64_3dHost.pushed, gpu64_3dHost.rejected, gpu64_3dHost.badOpcode };
	for ( unsigned i = 0; i < 3; i++ )
	{
		if ( i ) *p++ = '/';
		*p++ = hex[ ( v[ i ] >> 12 ) & 0xf ];
		*p++ = hex[ ( v[ i ] >>  8 ) & 0xf ];
		*p++ = hex[ ( v[ i ] >>  4 ) & 0xf ];
		*p++ = hex[   v[ i ]         & 0xf ];
	}

	*p++ = ' ';
	*p++ = 'c';
	*p++ = hex[ ( gpu64_3dWorkerStats.consumed >> 12 ) & 0xf ];
	*p++ = hex[ ( gpu64_3dWorkerStats.consumed >>  8 ) & 0xf ];
	*p++ = hex[ ( gpu64_3dWorkerStats.consumed >>  4 ) & 0xf ];
	*p++ = hex[   gpu64_3dWorkerStats.consumed         & 0xf ];

	// Arena use in KB, so an upload that silently failed to land is visible
	// without a second command.
	*p++ = ' ';
	*p++ = 'a';
	const u32 kb = gpu64_3dArenaUsed() >> 10;
	*p++ = hex[ ( kb >> 12 ) & 0xf ];
	*p++ = hex[ ( kb >>  8 ) & 0xf ];
	*p++ = hex[ ( kb >>  4 ) & 0xf ];
	*p++ = hex[   kb         & 0xf ];

	return p;
}

#endif	// GPU64_3D_ENABLED
