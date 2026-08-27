/*
 gpu64 milestone 6 -- resources, render state, and the rasteriser.

 This is the portable half of class 1: given a byte pointer to draw into, it
 transforms, lights, clips and rasterises. It knows nothing about IO2, the
 REU, the ring buffer, or which core it runs on, and it is compiled unchanged
 by tools/hostsim -- which is the point. Every pixel-level bug in the
 pipeline is findable on a PC.

 Formats and semantics: project/milestone6_3d_design.md. Not restated here.
*/
#ifndef _gpu64_3d_render_h
#define _gpu64_3d_render_h

#include <circle/types.h>
#include "gpu64_3d_math.h"
#include "gpu64_3d_span.h"

// --- limits -------------------------------------------------------------

#define GPU64_3D_MAX_VERTS	256		// the 1-byte face index
#define GPU64_3D_VERT_STRIDE	6		// x,y,z as 8.8 on the wire
#define GPU64_3D_FACE_STRIDE	12		// see Gpu64_3dFaceWire below

// The drawing surface, which the 3D layer shares with class 0.
#define GPU64_3D_SURFACE_W	320
#define GPU64_3D_SURFACE_H	200

// SET_VIEWPORT's ceiling: w*h*3 -- one colour byte plus two z bytes per
// pixel, which is what the render loop *writes*. Provisional per the design
// doc; the full surface (192000) fits under it.
#define GPU64_3D_BUDGET		196608

// Lighting levels the colormap carries. Level 15 is full brightness.
#define GPU64_3D_LIGHT_LEVELS	16
#define GPU64_3D_COLORMAP_BYTES	( GPU64_3D_LIGHT_LEVELS * 256 )

// --- wire formats -------------------------------------------------------

// Face blob element, exactly as it arrives. Byte 11 is padding to a stride
// the design fixed at 12: the eleven meaningful bytes would leave every
// other face straddling a cache line, and an exporter emitting a power-of-two
// stride is easier to write than one that does not.
struct Gpu64_3dFaceWire
{
	u8	i0, i1, i2;			// vertex indices into the vertex blob
	u8	u0, v0, u1, v1, u2, v2;		// per-corner texcoords
	u8	texid;				// low byte of a texture resource ID
	u8	flags;
	u8	pad;
};

#define GPU64_3D_FACE_DOUBLE_SIDED	0x01
#define GPU64_3D_FACE_FLAT_COLOUR	0x02	// texid is a palette index
#define GPU64_3D_FACE_UNLIT		0x04

// Unpacked face, as it is stored in the arena. The normal is computed once
// at upload from winding order (design doc, Mesh format) -- per frame it is
// only rotated, never recomputed, which is the difference between three
// multiplies and a cross product plus a square root per face per frame.
struct Gpu64_3dFace
{
	u8	i0, i1, i2;
	u8	u[ 3 ], v[ 3 ];
	u8	texid;
	u8	flags;
	s16	n[ 3 ];				// model-space normal, 1.15
};

struct Gpu64_3dMesh
{
	s16	*pVerts;			// 3 per vertex, 8.8 model space
	Gpu64_3dFace *pFaces;
	u16	nVerts;
	u16	nFaces;
	s16	centre[ 3 ];			// bounding sphere, 8.8 model space
	u16	radius;				// 8.8, unsigned
};

struct Gpu64_3dTexture
{
	u8	*pTexels;
	u8	wShift, hShift;			// dimensions are 1 << shift, 3..8
	u16	w, h;
};

// --- the resource table -------------------------------------------------
//
// One flat 16-bit ID namespace across every resource type, IDs assigned by
// the C64 (design doc, Resource lifecycle). A linear array rather than a hash:
// a lookup happens once per DRAW_MESH, not once per pixel, and a hash would
// add a failure mode -- a collision path -- to a structure whose whole job is
// to be inspectable when a mesh does not appear.

#define GPU64_3D_MAX_RESOURCES	512

#define GPU64_3D_RES_NONE	0
#define GPU64_3D_RES_MESH	1
#define GPU64_3D_RES_TEXTURE	2

struct Gpu64_3dResource
{
	u16	id;
	u8	type;
	u8	pad;
	Gpu64_3dMesh	mesh;
	Gpu64_3dTexture	tex;
};

// --- render state -------------------------------------------------------

struct Gpu64_3dState
{
	// viewport, in surface coordinates
	u16	vpX, vpY, vpW, vpH;

	// projection
	u16	fov;
	s32	focal;				// 16.16 pixels
	s32	nearZ, farZ;			// 16.16 view-space depths

	// the single directional light. Stored pointing *towards* the light,
	// which is the convention N.L assumes.
	s16	lightDir[ 3 ];			// 1.15
	u8	ambient;			// 0..15

	u8	background;

	// COLORMAP[level][index] -> index. 4 KB, so it stays L1-resident and
	// lighting costs one indexed byte load per pixel and zero DRAM traffic.
	u8	colormap[ GPU64_3D_COLORMAP_BYTES ];
	boolean	bColormapValid;

	// camera, as a view transform: rotate by this, then add this offset.
	Gpu64_3dMat	viewRot;
	Gpu64_3dVec	viewPos;		// camera position, world space, 16.16
	boolean		bHaveCamera;
};

// The surface being rendered into, plus the z-buffer that covers it.
struct Gpu64_3dTarget
{
	u8	*pPixels;			// top-left of the 320x200 surface
	unsigned pitch;
	u16	*pDepth;			// vpW * vpH, indexed viewport-relative
};

// --- entry points -------------------------------------------------------

// Puts the state back to its power-on defaults: full-surface viewport, 60
// degree fov, light from the front, ambient 4, background 0, colormap
// invalid. Called at boot and from every session reset.
void gpu64_3dStateDefaults( Gpu64_3dState *pState );

// Rebuilds the colormap from a 256-entry RGB palette (3 bytes an entry).
// This is BUILD_COLORMAP: 256 x 16 nearest-colour searches, a few
// milliseconds. Never call it per frame.
void gpu64_3dBuildColormap( Gpu64_3dState *pState, const u8 *pPaletteRGB );

// Fills the viewport with the background index and resets the z-buffer.
void gpu64_3dClearViewport( const Gpu64_3dState *pState, Gpu64_3dTarget *pTarget );

// Draws one mesh at a model transform. Returns the number of faces actually
// rasterised, which is the bring-up signal that says "the geometry arrived
// and was not entirely culled" -- distinguishable from "nothing was drawn"
// in a way a blank viewport is not.
//
// pLookup is how the rasteriser reaches the texture table without this file
// owning it; passing 0 renders every face as flat colour.
typedef const Gpu64_3dTexture *( *Gpu64_3dTextureLookup )( void *pCtx, u16 nId );

// Per-call scratch: the transformed vertices, one entry per mesh vertex. The
// caller owns it rather than this file holding a static, so that the day the
// render loop moves to core 1 there is no shared buffer to discover -- the
// two callers simply have two scratches. 3 KB.
struct Gpu64_3dScratch
{
	Gpu64_3dVec	view[ GPU64_3D_MAX_VERTS ];
};

unsigned gpu64_3dDrawMesh( const Gpu64_3dState *pState,
			   Gpu64_3dTarget *pTarget,
			   Gpu64_3dScratch *pScratch,
			   const Gpu64_3dMesh *pMesh,
			   const Gpu64_3dVec *pPos,	// 16.16 world space
			   const Gpu64_3dMat *pRot,
			   u16 nScale,			// unsigned 8.8
			   Gpu64_3dTextureLookup pLookup,
			   void *pLookupCtx );

// --- the rasteriser -----------------------------------------------------
// Exposed because the host sim exercises it directly, without a mesh.

struct Gpu64_3dRasterVert
{
	s32	sx, sy;				// 16.16 surface coordinates
	s32	invZ;				// 16.16, near/z -- 1.0 at the near plane
	s32	u, v;				// 16.16 texel coordinates
};

// Affine-mapped, z-tested, single light level for the whole triangle (flat
// shading, per the design's chosen look). pTex == 0 means flat colour, and
// nFlat is then the palette index.
void gpu64_3dRasterTriangle( const Gpu64_3dState *pState,
			     Gpu64_3dTarget *pTarget,
			     const Gpu64_3dRasterVert *pV,	// 3 of them
			     const Gpu64_3dTexture *pTex,
			     u8 nFlat,
			     u8 nLight );

// --- mesh and texture construction --------------------------------------
// Parse a blob pair / a texture blob into an arena block. pAlloc is the
// arena's bump allocator, passed in rather than called directly so the host
// sim can hand over plain malloc.
typedef void *( *Gpu64_3dAllocFn )( void *pCtx, u32 nBytes );

// Return values are GPU64_ERR_* codes from gpu64_api.h, repeated here so
// this file needs no firmware header. Kept numerically identical on purpose.
#define GPU64_3D_OK		0x00
#define GPU64_3D_BAD_ARGS	0x04
#define GPU64_3D_OUT_OF_MEMORY	0x08

u8 gpu64_3dBuildMesh( Gpu64_3dMesh *pMesh,
		      const u8 *pVertBlob, u32 nVertLen,
		      const u8 *pFaceBlob, u32 nFaceLen,
		      Gpu64_3dAllocFn pAlloc, void *pAllocCtx );

u8 gpu64_3dBuildTexture( Gpu64_3dTexture *pTex,
			 const u8 *pBlob, u32 nLen,
			 u8 nWShift, u8 nHShift,
			 Gpu64_3dAllocFn pAlloc, void *pAllocCtx );

#endif
