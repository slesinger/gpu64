/*
 gpu64 milestone 6 -- class 1: session state, the resource table, and the
 opcode dispatcher. Through Stage 14 every class 1 opcode, render ops
 included, ran synchronously on core 0. Stage 15a (project/gap_filling_plan.md)
 moves the three render ops -- CLEAR_VIEWPORT, DRAW_MESH, DRAW_NODE -- onto
 core 1, via the ring gpu64_3d_core1.cpp owns; everything else here is
 unchanged and still runs on core 0.

 DRAW_MESH is specified to return when the mesh is drawn and to be illegal
 while the render loop runs -- so there is exactly one owner of the
 framebuffer and the z-buffer at any instant. Stage 15a keeps that true by
 having gpu64_3dDispatch() stall on the ring fully draining before it returns
 for a render op: a zero-concurrency window, just relocated from "core 0,
 always" to "core 1, while core 0 waits" -- see gpu64_3dDispatch() below and
 gpu64-multicore-rule-scoped-to-polling-loop (memory) for why that stall is
 legal from core 0 despite the "never poll MMIO from another core" rule.

 gpu64_3dExecuteRender(), below, is the core-1 half of the three render ops --
 called from gpu64_3d_core1.cpp's execute(), it lives here because it needs
 the session state and resource table this file owns and does not export.
 Every other class 1 opcode is still pushed to the ring purely for its own
 counters (gpu64_3d_core1.cpp's unknownOp), then executed synchronously here
 on core 0, exactly as before Stage 15a.

 Nothing here is portable; the pipeline it drives (gpu64_3d_render.h) is, and
 is exercised on a PC by tools/hostsim.
*/
#include "gpu64_3d_internals.h"
#include "gpu64_3d_render.h"
#include "gpu64_3d_scene.h"
#include "gpu64_3d_span.h"
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

// Forward declaration: gpu64_3dReset(), directly below, needs Stage 15b's
// drain-before-mutate defined much further down (it comes after every
// opXxx() this file dispatches to, by design -- see the comment above its
// definition). Everything else in this file still relies on definition
// order rather than a declarations block; this is the one call that reaches
// backward across it.
static boolean drainAndFlush( void );

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
	// Stage 15b made this drain load-bearing rather than cosmetic: before
	// 15b every dispatch already drained the ring, so by the time a RESTORE
	// could reach here core 1 was always parked and idle. Now a render can
	// still be queued, or actually mid-execute on core 1, when RESTORE is
	// pressed -- and gpu64_3dExecuteRender() reads and writes exactly the
	// s_Res/s_State/s_Scene/s_Scratch/s_Depth structures this function is
	// about to reset. Draining first makes that impossible instead of
	// merely unlikely. The C64 being halted for this call still rules out a
	// *new* command arriving mid-reset -- that half of the old comment's
	// reasoning stands -- it just no longer covers one core 1 was already
	// working on when the halt began.
	//
	// Best-effort: if core 1 is genuinely wedged (the GPU64_3D_DRAIN_TIMEOUT_US
	// backstop), a RUN/STOP+RESTORE still has to recover the session rather
	// than leave the whole subsystem dead, so the reset proceeds regardless
	// of drainAndFlush()'s result -- there is no worse outcome available
	// from a void function with no caller to report a timeout to.
	drainAndFlush();

	// Only core 0 gets to say a new session's ring starts empty. Redundant
	// with the drain above whenever it succeeded (tail already == head by
	// then), kept as the statement of intent for the timeout case, where it
	// is the only thing that still moves the ring forward.
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
//
// Stage 15a: this now runs on core 1 (gpu64_3dExecuteRender(), below), not
// inside a dispatch that holds the C64's bus -- so the CLAUDE.md multicore
// burst cap applies to it for the first time. Before 15a a viewport's worth
// of rows was safe as one unchunked CleanDataCacheRange because core 0 owned
// the whole draw and the C64 was DMA-halted throughout; now it is core 1's
// own store burst, subject to the same 256-byte/7-line yield discipline as
// the rasteriser (gpu64_3d_span.h). One CleanRows() + one GPU64_3D_YIELD()
// per row, same as that header's own comment: "one yield per scanline...
// falls out of the geometry."
static void cleanViewport( void )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	if ( pFB == 0 )
		return;

	const u8 page = pFB->GetDrawPage();
	for ( u16 y = s_State.vpY; y < s_State.vpY + s_State.vpH; y++ )
	{
		pFB->CleanRows( page, y, y + 1 );
		GPU64_3D_YIELD();
	}
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

// --- Stage 15a: core-0-side prechecks for the three render opcodes ------
//
// gpu64_3dDispatch() (below) calls exactly one of these before it pushes a
// render opcode onto the ring. Everything that used to make opClearViewport/
// opDrawMesh/opDrawNode fail outright -- a bad resource id, a bad arg, no
// framebuffer -- must still be caught here, on core 0, before the command is
// ever queued: once it is on the ring core 1 runs it unconditionally (its own
// comment in gpu64_3d_core1.cpp says so), so a validation failure that
// reached the ring would either have to be invented a second failure path for
// or would silently execute on bad input. See gpu64-multicore-rule-scoped-to-
// polling-loop (memory) for why this split exists.

static u8 precheckClearViewport( void )
{
	if ( g_pGpu64FB == 0 || !g_pGpu64FB->IsInitialized() )
		return GPU64_ERR_UNSUPPORTED;
	return GPU64_ERR_OK;
}

static u8 precheckDrawMesh( void )
{
	if ( resFind( stagedId(), GPU64_3D_RES_MESH ) == 0 )
		return GPU64_ERR_BAD_ID;
	if ( g_pGpu64FB == 0 || !g_pGpu64FB->IsInitialized() )
		return GPU64_ERR_UNSUPPORTED;
	if ( argU16( 12 ) == 0 )				// scale
		return GPU64_ERR_BAD_ARGS;
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

// pNeedsDraw is set FALSE for a parked (SET_VISIBLE(0)) node: that is not an
// error -- it is DRAW_NODE's equivalent of DRAW_MESH's "every face culled"
// result -- but it means gpu64_3dDispatch() must not push anything onto the
// ring for it. RESULT is written here, directly, on core 0, for exactly that
// case; every other path through DRAW_NODE gets its RESULT from core 1's
// gpu64_3dExecuteRender() after the ring drains.
static u8 precheckDrawNode( boolean *pNeedsDraw )
{
	*pNeedsDraw = FALSE;

	Gpu64_3dNode *pN = gpu64_3dSceneFind( &s_Scene, stagedId(), GPU64_3D_NODE_OBJECT );
	if ( pN == 0 )
		return GPU64_ERR_BAD_ID;

	if ( !pN->visible )
	{
		gpu64Regs.result = 0;
		return GPU64_ERR_OK;
	}

	if ( resFind( pN->meshId, GPU64_3D_RES_MESH ) == 0 )
		return GPU64_ERR_BAD_ID;
	if ( g_pGpu64FB == 0 || !g_pGpu64FB->IsInitialized() )
		return GPU64_ERR_UNSUPPORTED;

	*pNeedsDraw = TRUE;
	return GPU64_ERR_OK;
}

// --- Stage 15a: core-1-side execution for the three render opcodes ------
//
// Called from gpu64_3d_core1.cpp's execute() -- core 1, not core 0 -- for
// CLEAR_VIEWPORT, DRAW_MESH and DRAW_NODE only, and only after the matching
// precheck*() above has already proved the command legal. pCmd is the ring
// slot itself; this writes pCmd->err and, for the two draws, pCmd->result,
// and never touches gpu64Regs -- core 0 remains the sole writer of the
// C64-visible register file, copying pCmd's fields across itself once its
// drain wait (waitForDrain(), below) succeeds.
//
// The self-warm blocks below are DRAW_NODE's own, moved here verbatim from
// the old opDrawNode() (CLAUDE.md rule 4): core 1 has its own, separate L1
// instruction cache, so a warm that ran on core 0 has no effect here, and
// this function is new enough on core 1 -- it has never executed before its
// first post-15a DRAW_NODE -- to need the same self-warm opDrawNode() did.
// DRAW_MESH's warm is new for the same reason: before 15a, opDrawMesh() had
// been kept warm by continuous use on core 0 since milestone 6, but that
// history is core-0 silicon and does not carry over to core 1's cache.
void gpu64_3dExecuteRender( Gpu64_3dCmd *pCmd )
{
	Gpu64_3dTarget target;

	switch ( pCmd->op )
	{
	case GPU64_3D_OP_CLEAR_VIEWPORT:
		if ( !makeTarget( &target ) )
		{
			// precheckClearViewport() already proved this can't happen.
			pCmd->err = GPU64_ERR_UNSUPPORTED;
			return;
		}
		gpu64_3dClearViewport( &s_State, &target );
		cleanViewport();
		pCmd->err = GPU64_ERR_OK;
		return;

	case GPU64_3D_OP_DRAW_MESH:
	{
		CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)gpu64_3dExecuteRender, 1024 * 2 );
		FORCE_READ_LINEARa( (void*)gpu64_3dExecuteRender, 1024 * 2, 1024 * 2 );

		const Gpu64_3dResource *pR = resFind( pCmd->id, GPU64_3D_RES_MESH );
		if ( pR == 0 || !makeTarget( &target ) )
		{
			// precheckDrawMesh() already proved this can't happen.
			pCmd->err = GPU64_ERR_BAD_ID;
			return;
		}

		Gpu64_3dVec pos;
		pos.x = (s32)(s16)( pCmd->arg[ 0 ] | ( pCmd->arg[ 1 ] << 8 ) ) << 8;	// 8.8 -> 16.16
		pos.y = (s32)(s16)( pCmd->arg[ 2 ] | ( pCmd->arg[ 3 ] << 8 ) ) << 8;
		pos.z = (s32)(s16)( pCmd->arg[ 4 ] | ( pCmd->arg[ 5 ] << 8 ) ) << 8;

		Gpu64_3dMat rot;
		const u16 yaw   = (u16)( pCmd->arg[ 6 ]  | ( pCmd->arg[ 7 ]  << 8 ) );
		const u16 pitch = (u16)( pCmd->arg[ 8 ]  | ( pCmd->arg[ 9 ]  << 8 ) );
		const u16 roll  = (u16)( pCmd->arg[ 10 ] | ( pCmd->arg[ 11 ] << 8 ) );
		gpu64_3dMatFromEuler( &rot, yaw, pitch, roll );

		const u16 scale = (u16)( pCmd->arg[ 12 ] | ( pCmd->arg[ 13 ] << 8 ) );

		const unsigned n = gpu64_3dDrawMesh( &s_State, &target, &s_Scratch, &pR->mesh,
						     &pos, &rot, scale, lookupTexture, 0 );
		cleanViewport();

		// RESULT is the triangle count, saturated to a byte -- see the
		// original opDrawMesh() comment this replaces: it is the difference
		// between "drawn, wrong part of the screen" and "every face culled".
		pCmd->result = (u8)( n > 255 ? 255 : n );
		pCmd->err = GPU64_ERR_OK;
		return;
	}

	case GPU64_3D_OP_DRAW_NODE:
	{
		CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)gpu64_3dExecuteRender, 1024 * 2 );
		FORCE_READ_LINEARa( (void*)gpu64_3dExecuteRender, 1024 * 2, 1024 * 2 );
		CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)gpu64_3dSceneFind, 1024 * 2 );
		FORCE_READ_LINEARa( (void*)gpu64_3dSceneFind, 1024 * 2, 1024 * 2 );
		CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)gpu64_3dSceneApplyCamera, 1024 * 2 );
		FORCE_READ_LINEARa( (void*)gpu64_3dSceneApplyCamera, 1024 * 2, 1024 * 2 );

		// Re-resolved by id rather than carried as a pointer from precheck:
		// precheck ran on core 0 before the push, this runs on core 1 after
		// the drain, and nothing about the node or the resource table is
		// locked between the two -- re-finding both here is what makes that
		// safe rather than merely convenient.
		Gpu64_3dNode *pN = gpu64_3dSceneFind( &s_Scene, pCmd->id, GPU64_3D_NODE_OBJECT );
		const Gpu64_3dResource *pR = pN ? resFind( pN->meshId, GPU64_3D_RES_MESH ) : 0;
		if ( pN == 0 || pR == 0 || !makeTarget( &target ) )
		{
			// precheckDrawNode() already proved this can't happen.
			pCmd->err = GPU64_ERR_BAD_ID;
			return;
		}

		// The active camera, if any, is applied fresh on every DRAW_NODE: a
		// program that moves the camera between two DRAW_NODE calls in the
		// same frame must see both nodes drawn from where the camera is
		// *now*, not from wherever it was at some earlier SET_ACTIVE_CAMERA.
		gpu64_3dSceneApplyCamera( &s_Scene, &s_State );

		const unsigned n = gpu64_3dDrawMesh( &s_State, &target, &s_Scratch, &pR->mesh,
						     &pN->pos, &pN->rot, pN->scale, lookupTexture, 0 );
		cleanViewport();

		pCmd->result = (u8)( n > 255 ? 255 : n );
		pCmd->err = GPU64_ERR_OK;
		return;
	}

	default:
		// Unreachable: gpu64_3dDispatch() only ever pushes the three render
		// ops above onto the ring for gpu64_3dExecuteRender() to see.
		pCmd->err = GPU64_ERR_BAD_OPCODE;
		return;
	}
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

	// CLEAR_VIEWPORT, DRAW_MESH and DRAW_NODE are not handled here as of
	// Stage 15a: gpu64_3dDispatch() intercepts all three before execute() is
	// ever called, running a precheck*() on core 0 and, once that passes,
	// pushing them to run on core 1 (gpu64_3dExecuteRender()). They fall
	// through to BAD_OPCODE below if execute() is ever reached with one,
	// which should not happen.

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

static boolean isRenderOp( u8 op )
{
	return op == GPU64_3D_OP_CLEAR_VIEWPORT
	    || op == GPU64_3D_OP_DRAW_MESH
	    || op == GPU64_3D_OP_DRAW_NODE;
}

// Generous on purpose: this is a backstop against a wedged core 1, not a
// tuned budget -- see project/gap_filling_plan.md's Stage 15 section. 50ms is
// a large multiple of any real draw this arena's size limit allows, and the
// cost of guessing too high is a slightly later ERRCODE on a build that was
// already broken; the cost of guessing too low is a spurious WORKER_TIMEOUT
// on a slow-but-fine frame.
#define GPU64_3D_DRAIN_TIMEOUT_US	50000

// Spins core 0 until the ring drains to head, or the timeout backstop fires.
// This is not the MMIO poll CLAUDE.md's multicore section forbids from a
// second core -- gpu64_3dRing.tail is a DRAM location, not a VideoCore
// peripheral register, and this runs on core 0 itself, watching core 1, which
// is the direction that rule never applied to. CNTVCT_EL0/CNTFRQ_EL0 rather
// than the BCM system timer for the same reason gpu64_ladder.cpp uses them:
// the only clock safe to read off core 0's own silicon.
static boolean waitForDrain( u32 head )
{
	u64 freq;
	asm volatile( "MRS %0, CNTFRQ_EL0" : "=r" (freq) );
	const u64 timeoutTicks = ( freq / 1000000ULL ) * GPU64_3D_DRAIN_TIMEOUT_US;

	u64 start;
	asm volatile( "MRS %0, CNTVCT_EL0" : "=r" (start) );

	while ( gpu64_3dRing.tail != head )
	{
		asm volatile( "YIELD" );

		u64 now;
		asm volatile( "MRS %0, CNTVCT_EL0" : "=r" (now) );
		if ( now - start > timeoutTicks )
			return gpu64_3dRing.tail == head;	// one last look before giving up
	}
	return TRUE;
}

// --- Stage 15b: observation points ---------------------------------------
//
// Under 15a every dispatch drained the ring before returning, so the ring
// never held more than one entry and "wait for the ring to empty" and "wait
// for this op's own completion" were the same thing. 15b lets CLEAR_VIEWPORT/
// DRAW_MESH/DRAW_NODE queue and return without waiting (below), which is the
// whole point -- a frame of DRAW_NODE calls now overlaps the C64 free-running
// instead of stalling on every one -- but it breaks that equivalence, and
// with it docs/class1-3d-mesh-reference.md's old "RESULT is valid the instant
// dispatch returns" contract for those three opcodes.
//
// What replaces it: RESULT is valid as of the last *observation point* --
// anywhere this file or gpu64_api.cpp forces the ring to fully drain before
// going on to do something that would otherwise race a still-in-flight
// render. That is every non-render class 1 opcode below (they mutate
// s_State/s_Scene/s_Res, which a queued render also reads), plus, from
// gpu64_api.cpp, a page flip/vsync commit and any class 0 opcode that touches
// the framebuffer -- see gpu64_3dSync() at the bottom of this section. A
// program that wants a DRAW_NODE's own RESULT right now (the debugging
// workflow docs/class1-3d-mesh-reference.md calls out: "check RESULT after
// your first upload of any new mesh") gets it by issuing any one of those as
// the next call and then reading RESULT -- it does not need a dedicated
// "sync" opcode, because ordinary programs already do this constantly.
//
// s_LastRenderSlot/s_LastRenderPending are core-0-private bookkeeping: which
// ring slot the most recently pushed render op landed in, and whether it has
// been flushed to gpu64Regs.result yet. Nothing on core 1 reads either.
static u32     s_LastRenderSlot;
static boolean s_LastRenderPending;

// Waits for the ring to fully drain, then -- if a render op was pushed since
// the last flush -- copies its RESULT across, exactly as gpu64_3dDispatch()
// used to do inline for every DRAW_MESH/DRAW_NODE before 15b. Returns FALSE
// only on the GPU64_3D_DRAIN_TIMEOUT_US backstop (a wedged core 1).
static boolean drainAndFlush( void )
{
	if ( !waitForDrain( gpu64_3dRing.head ) )
		return FALSE;

	// Pairs with the DMB gpu64_3dWorker() issues (gpu64_3d_core1.cpp) after
	// execute() writes err/result and before it publishes tail -- without
	// this, having observed tail == head does not by itself guarantee this
	// core sees the slot's new contents.
	asm volatile( "DMB ISH" ::: "memory" );

	if ( s_LastRenderPending )
	{
		const Gpu64_3dCmd *pSlot = &gpu64_3dRing.slot[ s_LastRenderSlot ];

		// Not for CLEAR_VIEWPORT: that opcode never touched RESULT before
		// Stage 15a either, and copying pSlot->result here would leak
		// whatever stale value a previous DRAW_MESH/DRAW_NODE left in this
		// slot (slots are a ring, reused, not per-opcode storage).
		if ( pSlot->op != GPU64_3D_OP_CLEAR_VIEWPORT )
			gpu64Regs.result = pSlot->result;
		s_LastRenderPending = FALSE;
	}
	return TRUE;
}

// gpu64_api.cpp's observation-point hook: a page flip/vsync commit or a
// class 0 opcode that touches the framebuffer must not run ahead of a
// render this file has queued but not yet executed on core 1 -- see the
// comment above drainAndFlush(). A no-op (returns TRUE immediately) whenever
// GPU64_3D_ENABLED is off, so callers do not need their own #ifdef for the
// common case; they still need one around the call itself, the same as
// every other gpu64_3d.h entry point, because this whole file is compiled
// out with the toggle.
boolean gpu64_3dSync( void )
{
	return drainAndFlush();
}

u8 gpu64_3dDispatch( u8 op )
{
	u8 res;

	// gpu64_3dHost.pushed is documented as "commands accepted onto the ring"
	// (gpu64_3d_internals.h). Before Stage 15a that was every non-QUEUE_FULL
	// dispatch, because every accepted op was pushed unconditionally, before
	// validation. Hazard #2's fix (project/gap_filling_plan.md, Stage 15)
	// means a render op that fails its precheck, or a parked DRAW_NODE that
	// needs no draw, now never touches the ring at all -- didPush tracks
	// that exactly, so the counter keeps meaning what its comment says.
	boolean didPush = FALSE;

	if ( isRenderOp( op ) )
	{
		// Stage 15a: precheck on core 0 before the command ever reaches the
		// ring -- see the precheck*() comment above for why this order
		// matters now that core 1 actually executes what it drains, instead
		// of only counting it.
		boolean needsDraw = TRUE;

		if ( op == GPU64_3D_OP_CLEAR_VIEWPORT )
			res = precheckClearViewport();
		else if ( op == GPU64_3D_OP_DRAW_MESH )
			res = precheckDrawMesh();
		else
			res = precheckDrawNode( &needsDraw );

		if ( res == GPU64_ERR_OK && needsDraw )
		{
			u32 slot = gpu64_3dRing.head;

			if ( !gpu64_3dRingPush( op ) )
			{
				// QUEUE_FULL backpressure (Stage 15b): wait for the ring to
				// fully drain, bus held, rather than reject outright -- a
				// C64 program drawing faster than core 1 renders should see
				// its draws queue, not fail. gap_filling_plan.md's Stage 15b
				// QUEUE_FULL note prefers this to an outright reject, with
				// GPU64_3D_DRAIN_TIMEOUT_US as the backstop against a
				// wedged core 1. A ring that has just fully drained cannot
				// immediately be full again -- core 0 is the only producer,
				// and it is not doing anything else meanwhile -- so the
				// retried push below is not itself allowed to fail.
				if ( !drainAndFlush() )
				{
					gpu64_3dHost.rejected++;
					res = GPU64_ERR_WORKER_TIMEOUT;
				}
				else
				{
					slot = gpu64_3dRing.head;
					if ( gpu64_3dRingPush( op ) )
					{
						didPush = TRUE;
					}
					else
					{
						// Unreachable in practice -- a ring that just fully
						// drained cannot be full again one push later, core
						// 0 being the only producer -- but fail safely
						// rather than assume it.
						gpu64_3dHost.rejected++;
						res = GPU64_ERR_QUEUE_FULL;
					}
				}
			}
			else
			{
				didPush = TRUE;
			}

			if ( didPush )
			{
				// Stage 15b: push and return -- this draw's own completion
				// is not waited for here. RESULT/ERRCODE for *this specific
				// call* become valid at the next observation point; see the
				// comment above drainAndFlush().
				s_LastRenderSlot = slot;
				s_LastRenderPending = TRUE;
			}
		}
	}
	else
	{
		// The ring is fed first, and a full ring refuses the command
		// outright: the design's rule is that a failed dispatch does
		// nothing, so a command must not execute and then be reported as
		// rejected.
		if ( !gpu64_3dRingPush( op ) )
		{
			gpu64_3dHost.rejected++;
			return GPU64_ERR_QUEUE_FULL;
		}
		didPush = TRUE;

		// Stage 15b: execute(), below, is about to mutate s_State/s_Scene/
		// s_Res synchronously on core 0 -- and a render queued earlier, not
		// yet executed, reads exactly that state on core 1. Draining first
		// turns this from a race into an observation point: a draw issued
		// before this dispatch always sees the state that was live when
		// *it* was issued, and this dispatch's own mutation is only visible
		// to draws issued after it returns. This has the same ordering
		// effect as literally sequencing every one of these ~15 opcodes
		// through the ring the way the three render ops are, without adding
		// a second, core-1-side execution path for opcodes that still only
		// ever run on core 0 -- gap_filling_plan.md's Stage 15b "sequence
		// the state opcodes through the ring too" note.
		if ( !drainAndFlush() )
			return GPU64_ERR_WORKER_TIMEOUT;

		res = execute( op );
	}

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
	else if ( didPush )
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
