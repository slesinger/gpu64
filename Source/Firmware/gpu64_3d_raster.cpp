/*
 gpu64 milestone 6 -- the affine, z-tested triangle rasteriser, and the
 viewport clear. See gpu64_3d_render.h.

 Portable: compiled unchanged by the firmware and by tools/hostsim.
*/
#include "gpu64_3d_render.h"

// gpu64: three bytes leave the core per pixel -- one colour, two z -- so the
// milestone 6a burst budget is this many pixels, not this many bytes. The
// yield sits inside the span loop rather than at the end of a scanline
// because a full-width span is 960 bytes, nearly four times the budget.
#define GPU64_3D_SPAN_PIXELS	( GPU64_3D_SPAN_BYTES / 3 )

typedef __int128 s128;

// --- viewport clear -----------------------------------------------------

void gpu64_3dClearViewport( const Gpu64_3dState *pState, Gpu64_3dTarget *pTarget )
{
	const unsigned w = pState->vpW;
	const unsigned h = pState->vpH;

	for ( unsigned y = 0; y < h; y++ )
	{
		u8  *pRow = pTarget->pPixels + (size_t)( pState->vpY + y ) * pTarget->pitch + pState->vpX;
		u16 *pZ   = pTarget->pDepth + (size_t)y * w;

		unsigned x = 0;
		while ( x < w )
		{
			unsigned n = w - x;
			if ( n > GPU64_3D_SPAN_PIXELS )
				n = GPU64_3D_SPAN_PIXELS;

			for ( unsigned i = 0; i < n; i++ )
			{
				pRow[ x + i ] = pState->background;
				// 0 is "infinitely far": the buffer holds near/z, so bigger
				// is nearer and every real fragment beats an untouched pixel.
				pZ[ x + i ] = 0;
			}

			x += n;
			GPU64_3D_YIELD();
		}
	}
}

// --- the rasteriser -----------------------------------------------------

struct Gpu64_3dGrad
{
	s32	dx, dy;				// attribute change per pixel, 16.16
};

// Plane gradients of one attribute over the triangle. Done in 128-bit
// because the numerator is a 16.16 attribute times a 16.16 coordinate
// difference and the result then has to be scaled back up by 65536 before
// the divide -- doing that in 64 bits overflows for any triangle bigger than
// a few pixels, and the symptom is a texture that shears only on large
// polygons, which is a miserable thing to chase.
static void gradient( Gpu64_3dGrad *pG, s64 d,
		      s64 a0, s64 a1, s64 a2,
		      s64 dx1, s64 dy1, s64 dx2, s64 dy2 )
{
	const s64 da1 = a1 - a0;
	const s64 da2 = a2 - a0;

	const s128 numX = ( (s128)da1 * dy2 - (s128)da2 * dy1 ) << 16;
	const s128 numY = ( (s128)da2 * dx1 - (s128)da1 * dx2 ) << 16;

	pG->dx = (s32)( numX / d );
	pG->dy = (s32)( numY / d );
}

void gpu64_3dRasterTriangle( const Gpu64_3dState *pState,
			     Gpu64_3dTarget *pTarget,
			     const Gpu64_3dRasterVert *pV,
			     const Gpu64_3dTexture *pTex,
			     u8 nFlat,
			     u8 nLight )
{
	const s64 dx1 = pV[ 1 ].sx - pV[ 0 ].sx;
	const s64 dy1 = pV[ 1 ].sy - pV[ 0 ].sy;
	const s64 dx2 = pV[ 2 ].sx - pV[ 0 ].sx;
	const s64 dy2 = pV[ 2 ].sy - pV[ 0 ].sy;

	const s64 d = dx1 * dy2 - dx2 * dy1;
	if ( d == 0 )
		return;					// zero area: nothing to fill

	Gpu64_3dGrad gz, gu, gv;
	gradient( &gz, d, pV[ 0 ].invZ, pV[ 1 ].invZ, pV[ 2 ].invZ, dx1, dy1, dx2, dy2 );
	gradient( &gu, d, pV[ 0 ].u, pV[ 1 ].u, pV[ 2 ].u, dx1, dy1, dx2, dy2 );
	gradient( &gv, d, pV[ 0 ].v, pV[ 1 ].v, pV[ 2 ].v, dx1, dy1, dx2, dy2 );

	// Vertices sorted by y, as three indices -- the vertex data itself is
	// never moved, so the gradients above stay keyed to vertex 0.
	unsigned i0 = 0, i1 = 1, i2 = 2;
	if ( pV[ i0 ].sy > pV[ i1 ].sy ) { unsigned t = i0; i0 = i1; i1 = t; }
	if ( pV[ i1 ].sy > pV[ i2 ].sy ) { unsigned t = i1; i1 = i2; i2 = t; }
	if ( pV[ i0 ].sy > pV[ i1 ].sy ) { unsigned t = i0; i0 = i1; i1 = t; }

	const int vpX0 = pState->vpX;
	const int vpY0 = pState->vpY;
	const int vpX1 = pState->vpX + pState->vpW;
	const int vpY1 = pState->vpY + pState->vpH;

	// Pixel centres are at (x + 0.5, y + 0.5); a scanline is covered when its
	// centre falls inside the edge span. (v + 32767) >> 16 is the ceiling of
	// (v/65536 - 0.5), i.e. the first covered pixel.
	int y0 = (int)( ( pV[ i0 ].sy + 32767 ) >> 16 );
	int y1 = (int)( ( pV[ i2 ].sy + 32767 ) >> 16 );

	if ( y0 < vpY0 ) y0 = vpY0;
	if ( y1 > vpY1 ) y1 = vpY1;

	const u8 *pColormap = pState->colormap + (size_t)nLight * 256;
	const u8 flatIndex  = pColormap[ nFlat ];

	const u32 uMask = pTex ? (u32)( pTex->w - 1 ) : 0;
	const u32 vMask = pTex ? (u32)( pTex->h - 1 ) : 0;
	const u8  wShift = pTex ? pTex->wShift : 0;

	for ( int y = y0; y < y1; y++ )
	{
		const s64 yc = ( (s64)y << 16 ) + 32768;

		// The long edge i0->i2 always spans the whole triangle; which short
		// edge is active depends on whether we are above or below vertex i1.
		s64 xa, xb;

		{
			const s64 den = pV[ i2 ].sy - pV[ i0 ].sy;
			if ( den == 0 )
				continue;
			xa = pV[ i0 ].sx + ( ( pV[ i2 ].sx - pV[ i0 ].sx ) * ( yc - pV[ i0 ].sy ) ) / den;
		}

		if ( yc < pV[ i1 ].sy )
		{
			const s64 den = pV[ i1 ].sy - pV[ i0 ].sy;
			if ( den == 0 )
				continue;
			xb = pV[ i0 ].sx + ( ( pV[ i1 ].sx - pV[ i0 ].sx ) * ( yc - pV[ i0 ].sy ) ) / den;
		} else
		{
			const s64 den = pV[ i2 ].sy - pV[ i1 ].sy;
			if ( den == 0 )
				continue;
			xb = pV[ i1 ].sx + ( ( pV[ i2 ].sx - pV[ i1 ].sx ) * ( yc - pV[ i1 ].sy ) ) / den;
		}

		s64 xl = xa < xb ? xa : xb;
		s64 xr = xa < xb ? xb : xa;

		int x0 = (int)( ( xl + 32767 ) >> 16 );
		int x1 = (int)( ( xr + 32767 ) >> 16 );

		if ( x0 < vpX0 ) x0 = vpX0;
		if ( x1 > vpX1 ) x1 = vpX1;
		if ( x0 >= x1 )
			continue;

		// Attribute values at the centre of the span's first pixel, then a
		// plain add per pixel. This is the affine mapping the design chose:
		// no per-pixel divide, and the price is the texture swimming on
		// near-parallel surfaces, which boxy low-poly geometry does not show.
		const s64 fx = ( ( (s64)x0 << 16 ) + 32768 ) - pV[ 0 ].sx;
		const s64 fy = yc - pV[ 0 ].sy;

		s32 z = (s32)( pV[ 0 ].invZ + ( ( (s64)gz.dx * fx + (s64)gz.dy * fy ) >> 16 ) );
		s32 u = (s32)( pV[ 0 ].u    + ( ( (s64)gu.dx * fx + (s64)gu.dy * fy ) >> 16 ) );
		s32 v = (s32)( pV[ 0 ].v    + ( ( (s64)gv.dx * fx + (s64)gv.dy * fy ) >> 16 ) );

		u8  *pRow = pTarget->pPixels + (size_t)y * pTarget->pitch;
		u16 *pZ   = pTarget->pDepth + (size_t)( y - vpY0 ) * pState->vpW - vpX0;

		int x = x0;
		while ( x < x1 )
		{
			int n = x1 - x;
			if ( n > GPU64_3D_SPAN_PIXELS )
				n = GPU64_3D_SPAN_PIXELS;

			for ( int i = 0; i < n; i++, x++ )
			{
				// invZ is 1.0 at the near plane and falls off with distance,
				// so the test is >, not <, and an untouched pixel (0) always
				// loses.
				u32 depth = (u32)( z < 0 ? 0 : z );
				if ( depth > 65535 ) depth = 65535;

				if ( depth > pZ[ x ] )
				{
					if ( pTex )
					{
						const u32 tu = ( (u32)( u >> 16 ) ) & uMask;
						const u32 tv = ( (u32)( v >> 16 ) ) & vMask;
						pRow[ x ] = pColormap[ pTex->pTexels[ ( tv << wShift ) | tu ] ];
					} else
						pRow[ x ] = flatIndex;

					pZ[ x ] = (u16)depth;
				}

				z += gz.dx;
				u += gu.dx;
				v += gv.dx;
			}

			GPU64_3D_YIELD();
		}
	}
}
