/*
 gpu64 milestone 6 -- mesh and texture construction, and the per-mesh draw:
 transform, cull, light, near-clip, project, rasterise.

 Portable: compiled unchanged by the firmware and by tools/hostsim.

 Coordinates are left-handed -- +x right, +y up, +z away from the camera --
 and a face is front-facing when its normal points back towards the camera.
 Winding is therefore clockwise as seen from outside the model, which is what
 an exporter has to emit; get it backwards and every face of a closed mesh is
 culled, which looks exactly like "the upload did not arrive". DRAW_MESH's
 return count is what tells those two apart.
*/
#include "gpu64_3d_render.h"

// --- mesh construction --------------------------------------------------

u8 gpu64_3dBuildMesh( Gpu64_3dMesh *pMesh,
		      const u8 *pVertBlob, u32 nVertLen,
		      const u8 *pFaceBlob, u32 nFaceLen,
		      Gpu64_3dAllocFn pAlloc, void *pAllocCtx )
{
	if ( nVertLen == 0 || ( nVertLen % GPU64_3D_VERT_STRIDE ) != 0 )
		return GPU64_3D_BAD_ARGS;
	if ( nFaceLen == 0 || ( nFaceLen % GPU64_3D_FACE_STRIDE ) != 0 )
		return GPU64_3D_BAD_ARGS;

	const u32 nVerts = nVertLen / GPU64_3D_VERT_STRIDE;
	const u32 nFaces = nFaceLen / GPU64_3D_FACE_STRIDE;

	if ( nVerts > GPU64_3D_MAX_VERTS )
		return GPU64_3D_BAD_ARGS;		// the 1-byte face index

	s16 *pVerts = (s16 *)pAlloc( pAllocCtx, nVerts * 3 * sizeof( s16 ) );
	if ( !pVerts )
		return GPU64_3D_OUT_OF_MEMORY;

	Gpu64_3dFace *pFaces = (Gpu64_3dFace *)pAlloc( pAllocCtx, nFaces * sizeof( Gpu64_3dFace ) );
	if ( !pFaces )
		return GPU64_3D_OUT_OF_MEMORY;

	s32 minv[ 3 ] = {  32767,  32767,  32767 };
	s32 maxv[ 3 ] = { -32768, -32768, -32768 };

	for ( u32 i = 0; i < nVerts; i++ )
		for ( unsigned k = 0; k < 3; k++ )
		{
			const s16 c = (s16)( pVertBlob[ i * 6 + k * 2 ] | ( pVertBlob[ i * 6 + k * 2 + 1 ] << 8 ) );
			pVerts[ i * 3 + k ] = c;
			if ( c < minv[ k ] ) minv[ k ] = c;
			if ( c > maxv[ k ] ) maxv[ k ] = c;
		}

	for ( u32 f = 0; f < nFaces; f++ )
	{
		const u8 *pW = pFaceBlob + f * GPU64_3D_FACE_STRIDE;
		Gpu64_3dFace *pF = &pFaces[ f ];

		pF->i0 = pW[ 0 ];
		pF->i1 = pW[ 1 ];
		pF->i2 = pW[ 2 ];

		// An out-of-range index is rejected at upload, not clamped at draw
		// time: a mesh that indexes past its own vertex blob is a broken
		// export, and silently drawing something plausible would hide it
		// until it read a texture pointer out of the arena.
		if ( pF->i0 >= nVerts || pF->i1 >= nVerts || pF->i2 >= nVerts )
			return GPU64_3D_BAD_ARGS;

		pF->u[ 0 ] = pW[ 3 ]; pF->v[ 0 ] = pW[ 4 ];
		pF->u[ 1 ] = pW[ 5 ]; pF->v[ 1 ] = pW[ 6 ];
		pF->u[ 2 ] = pW[ 7 ]; pF->v[ 2 ] = pW[ 8 ];
		pF->texid  = pW[ 9 ];
		pF->flags  = pW[ 10 ];

		const s16 *a = &pVerts[ pF->i0 * 3 ];
		const s16 *b = &pVerts[ pF->i1 * 3 ];
		const s16 *c = &pVerts[ pF->i2 * 3 ];

		const s64 e1x = b[ 0 ] - a[ 0 ], e1y = b[ 1 ] - a[ 1 ], e1z = b[ 2 ] - a[ 2 ];
		const s64 e2x = c[ 0 ] - a[ 0 ], e2y = c[ 1 ] - a[ 1 ], e2z = c[ 2 ] - a[ 2 ];

		if ( !gpu64_3dNormalise( pF->n,
					 e1y * e2z - e1z * e2y,
					 e1z * e2x - e1x * e2z,
					 e1x * e2y - e1y * e2x ) )
		{
			// A collapsed face has no normal. It is kept rather than
			// rejected -- exporters emit degenerate triangles for all sorts
			// of reasons -- and given a normal that faces the camera, so it
			// lights sanely on the frames where it is not zero-area.
			pF->n[ 0 ] = 0;
			pF->n[ 1 ] = 0;
			pF->n[ 2 ] = -( GPU64_FX15_ONE - 1 );
		}
	}

	// Bounding sphere over the AABB centre. Not the minimal sphere, which
	// would be a Welzl pass for a couple of percent on a cull test that is
	// already conservative.
	s64 r2 = 0;
	for ( unsigned k = 0; k < 3; k++ )
		pMesh->centre[ k ] = (s16)( ( minv[ k ] + maxv[ k ] ) / 2 );

	for ( u32 i = 0; i < nVerts; i++ )
	{
		s64 d2 = 0;
		for ( unsigned k = 0; k < 3; k++ )
		{
			const s64 d = pVerts[ i * 3 + k ] - pMesh->centre[ k ];
			d2 += d * d;
		}
		if ( d2 > r2 ) r2 = d2;
	}

	u32 r = gpu64_3dSqrt64( (u64)r2 ) + 1;		// +1 covers the rounding down
	if ( r > 65535 ) r = 65535;

	pMesh->pVerts = pVerts;
	pMesh->pFaces = pFaces;
	pMesh->nVerts = (u16)nVerts;
	pMesh->nFaces = (u16)nFaces;
	pMesh->radius = (u16)r;

	return GPU64_3D_OK;
}

u8 gpu64_3dBuildTexture( Gpu64_3dTexture *pTex,
			 const u8 *pBlob, u32 nLen,
			 u8 nWShift, u8 nHShift,
			 Gpu64_3dAllocFn pAlloc, void *pAllocCtx )
{
	// 3..8 on each side, i.e. 8 to 256 texels. Power-of-two is what makes the
	// inner loop's wrap a mask instead of a modulo, and 256 is the ceiling
	// the byte texcoords already imply.
	if ( nWShift < 3 || nWShift > 8 || nHShift < 3 || nHShift > 8 )
		return GPU64_3D_BAD_ARGS;

	const u32 w = 1u << nWShift;
	const u32 h = 1u << nHShift;

	if ( nLen != w * h )
		return GPU64_3D_BAD_ARGS;

	u8 *pTexels = (u8 *)pAlloc( pAllocCtx, nLen );
	if ( !pTexels )
		return GPU64_3D_OUT_OF_MEMORY;

	for ( u32 i = 0; i < nLen; i++ )
		pTexels[ i ] = pBlob[ i ];

	pTex->pTexels = pTexels;
	pTex->wShift  = nWShift;
	pTex->hShift  = nHShift;
	pTex->w       = (u16)w;
	pTex->h       = (u16)h;

	return GPU64_3D_OK;
}

// --- the draw -----------------------------------------------------------

// A vertex mid-pipeline: view-space position plus the texcoords that travel
// with it through the near clip.
struct ClipVert
{
	Gpu64_3dVec	p;
	s32		u, v;				// 16.16 texel coordinates
};

static void lerpVert( ClipVert *pOut, const ClipVert *pA, const ClipVert *pB, s64 num, s64 den )
{
	// t = num/den, computed per component in 64 bits rather than as a
	// precomputed 16.16 t: a triangle nearly parallel to the near plane makes
	// den tiny, and rounding t first puts the clipped vertex visibly off the
	// plane.
	#define LERP( a, b )	( (s32)( (s64)(a) + ( ( (s64)(b) - (s64)(a) ) * num ) / den ) )

	pOut->p.x = LERP( pA->p.x, pB->p.x );
	pOut->p.y = LERP( pA->p.y, pB->p.y );
	pOut->p.z = LERP( pA->p.z, pB->p.z );
	pOut->u   = LERP( pA->u,   pB->u   );
	pOut->v   = LERP( pA->v,   pB->v   );

	#undef LERP
}

// Sutherland-Hodgman against the single near plane, z >= nearZ. Near-plane
// only, with the viewport rect handled by the rasteriser's scissor -- design
// doc, open question 3: the cheap answer, and the one that keeps the clipper
// at a fixed 4-vertex maximum.
static unsigned clipNear( ClipVert *pOut, const ClipVert *pIn, unsigned n, s32 nearZ )
{
	unsigned nOut = 0;

	for ( unsigned i = 0; i < n; i++ )
	{
		const ClipVert *pA = &pIn[ i ];
		const ClipVert *pB = &pIn[ ( i + 1 ) % n ];

		const boolean bAIn = pA->p.z >= nearZ;
		const boolean bBIn = pB->p.z >= nearZ;

		if ( bAIn )
			pOut[ nOut++ ] = *pA;

		if ( bAIn != bBIn )
			lerpVert( &pOut[ nOut++ ], pA, pB, (s64)nearZ - pA->p.z, (s64)pB->p.z - pA->p.z );
	}

	return nOut;
}

static void project( Gpu64_3dRasterVert *pOut, const ClipVert *pIn,
		     const Gpu64_3dState *pState )
{
	const s64 z = pIn->p.z;

	const s64 cx = ( (s64)pState->vpX + pState->vpW / 2 ) << 16;
	const s64 cy = ( (s64)pState->vpY + pState->vpH / 2 ) << 16;

	pOut->sx = (s32)( cx + ( (s64)pIn->p.x * pState->focal ) / z );
	// Screen y grows downwards and world y grows up, so the projection
	// subtracts. Getting this backwards produces a scene that renders
	// perfectly and is upside down, which is a slow thing to notice on a
	// symmetric test cube -- hence the asymmetric cube in tools/hostsim.
	pOut->sy = (s32)( cy - ( (s64)pIn->p.y * pState->focal ) / z );

	// near/z, so the value is 1.0 (65536) at the near plane and falls off
	// with distance. Linear in screen space, which is what makes it
	// interpolable by the affine rasteriser.
	pOut->invZ = (s32)( ( (s64)pState->nearZ << 16 ) / z );

	pOut->u = pIn->u;
	pOut->v = pIn->v;
}

unsigned gpu64_3dDrawMesh( const Gpu64_3dState *pState,
			   Gpu64_3dTarget *pTarget,
			   Gpu64_3dScratch *pScratch,
			   const Gpu64_3dMesh *pMesh,
			   const Gpu64_3dVec *pPos,
			   const Gpu64_3dMat *pRot,
			   u16 nScale,
			   Gpu64_3dTextureLookup pLookup,
			   void *pLookupCtx )
{
	unsigned nDrawn = 0;

	if ( pMesh->nVerts == 0 || pMesh->nFaces == 0 || pState->focal <= 0 )
		return 0;

	// One rotation for the whole draw: view * model. Composing here rather
	// than rotating twice per vertex halves the per-vertex cost and is the
	// only reason the view transform is kept as a rotation plus an offset
	// instead of a 4x4.
	Gpu64_3dMat total;
	gpu64_3dMatMul( &total, &pState->viewRot, pRot );

	// The model's origin in view space.
	Gpu64_3dVec rel, origin;
	rel.x = pPos->x - pState->viewPos.x;
	rel.y = pPos->y - pState->viewPos.y;
	rel.z = pPos->z - pState->viewPos.z;
	gpu64_3dVecRotate( &origin, &pState->viewRot, &rel );

	// Bounding-sphere reject against the near and far planes. Cheap, and it
	// is what makes splitting large geometry across meshes pay -- design doc,
	// Mesh format.
	{
		const s64 radius = ( (s64)pMesh->radius * nScale ) >> 8;	// 8.8 * 8.8 -> 8.8
		Gpu64_3dVec c, cv;
		c.x = ( (s32)pMesh->centre[ 0 ] * nScale );		// 8.8 * 8.8 -> 16.16
		c.y = ( (s32)pMesh->centre[ 1 ] * nScale );
		c.z = ( (s32)pMesh->centre[ 2 ] * nScale );
		gpu64_3dVecRotate( &cv, &total, &c );

		const s64 cz = (s64)cv.z + origin.z;
		const s64 r16 = radius << 8;				// 8.8 -> 16.16
		if ( cz + r16 < pState->nearZ )
			return 0;
		if ( cz - r16 > pState->farZ )
			return 0;
	}

	for ( unsigned i = 0; i < pMesh->nVerts; i++ )
	{
		Gpu64_3dVec m, r;

		// 8.8 model coordinate times 8.8 scale is exactly 16.16, with no
		// shift and no rounding -- the one place in this pipeline where the
		// fixed-point formats line up for free.
		m.x = (s32)pMesh->pVerts[ i * 3 + 0 ] * nScale;
		m.y = (s32)pMesh->pVerts[ i * 3 + 1 ] * nScale;
		m.z = (s32)pMesh->pVerts[ i * 3 + 2 ] * nScale;

		gpu64_3dVecRotate( &r, &total, &m );

		pScratch->view[ i ].x = r.x + origin.x;
		pScratch->view[ i ].y = r.y + origin.y;
		pScratch->view[ i ].z = r.z + origin.z;
	}

	// The light lives in world space; the pipeline works in view space, so
	// rotate it once here rather than rotating every normal back.
	s16 lightView[ 3 ];
	gpu64_3dNormalRotate( lightView, &pState->viewRot, pState->lightDir );

	for ( unsigned f = 0; f < pMesh->nFaces; f++ )
	{
		const Gpu64_3dFace *pF = &pMesh->pFaces[ f ];

		s16 n[ 3 ];
		gpu64_3dNormalRotate( n, &total, pF->n );

		const Gpu64_3dVec *pV0 = &pScratch->view[ pF->i0 ];

		// Backface test against the vector from the camera (at the view-space
		// origin) to the face. Not against the z axis: with a wide fov a face
		// at the edge of the viewport can face away from the axis and still
		// be visible.
		const s64 facing = ( (s64)n[ 0 ] * pV0->x + (s64)n[ 1 ] * pV0->y + (s64)n[ 2 ] * pV0->z );

		boolean bFlip = FALSE;
		if ( facing >= 0 )
		{
			if ( !( pF->flags & GPU64_3D_FACE_DOUBLE_SIDED ) )
				continue;
			bFlip = TRUE;			// light the side actually being seen
		}

		u8 light = GPU64_3D_LIGHT_LEVELS - 1;
		if ( !( pF->flags & GPU64_3D_FACE_UNLIT ) )
		{
			s16 ln[ 3 ] = { n[ 0 ], n[ 1 ], n[ 2 ] };
			if ( bFlip )
			{
				ln[ 0 ] = (s16)-ln[ 0 ];
				ln[ 1 ] = (s16)-ln[ 1 ];
				ln[ 2 ] = (s16)-ln[ 2 ];
			}

			s32 ndotl = gpu64_3dDot15( ln, lightView );
			if ( ndotl < 0 ) ndotl = 0;

			const u32 span = ( GPU64_3D_LIGHT_LEVELS - 1 ) - pState->ambient;
			u32 lit = pState->ambient + ( ( (u32)ndotl * span ) >> GPU64_FX15_SHIFT );
			if ( lit > GPU64_3D_LIGHT_LEVELS - 1 )
				lit = GPU64_3D_LIGHT_LEVELS - 1;
			light = (u8)lit;
		}

		ClipVert in[ 3 ], out[ 8 ];
		const u8 idx[ 3 ] = { pF->i0, pF->i1, pF->i2 };

		for ( unsigned k = 0; k < 3; k++ )
		{
			in[ k ].p = pScratch->view[ idx[ k ] ];
			// A texcoord byte is a texel coordinate directly, wrapped by the
			// rasteriser's mask -- so 0..255 addresses a 256-wide texture
			// exactly and tiles a narrower one.
			in[ k ].u = (s32)pF->u[ k ] << 16;
			in[ k ].v = (s32)pF->v[ k ] << 16;
		}

		const unsigned nOut = clipNear( out, in, 3, pState->nearZ );
		if ( nOut < 3 )
			continue;

		const Gpu64_3dTexture *pTex = 0;
		u8 flat = pF->texid;

		if ( !( pF->flags & GPU64_3D_FACE_FLAT_COLOUR ) && pLookup )
			pTex = pLookup( pLookupCtx, pF->texid );

		// Fan the clipped polygon. At most 4 vertices come out of a single
		// plane clip of a triangle, so this is one or two triangles.
		for ( unsigned t = 1; t + 1 < nOut; t++ )
		{
			Gpu64_3dRasterVert rv[ 3 ];
			project( &rv[ 0 ], &out[ 0 ],     pState );
			project( &rv[ 1 ], &out[ t ],     pState );
			project( &rv[ 2 ], &out[ t + 1 ], pState );

			gpu64_3dRasterTriangle( pState, pTarget, rv, pTex, flat, light );
			nDrawn++;
		}
	}

	return nDrawn;
}
