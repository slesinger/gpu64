/*
 gpu64 host sim -- drives the portable half of the milestone 6 renderer with
 a native g++, and writes what it produces out as PPM images.

 Why this exists: every pixel-level bug in the 3D pipeline -- a transposed
 matrix, an inverted winding, a texture that shears, a z test with the
 comparison the wrong way round -- is findable here in a second, and costs an
 SD card swap, a power cycle and a trip to the bench if it is instead found on
 hardware. The firmware sources are compiled unchanged; only <circle/types.h>
 is stubbed (stub/circle/).

 Usage: ./hostsim [outdir]
*/
#include "gpu64_3d_render.h"
#include "gpu64_3d_scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// --- a heap for the arena callback --------------------------------------

static void *hostAlloc( void *, u32 nBytes )
{
	return malloc( nBytes );
}

// --- the surface --------------------------------------------------------

static u8  g_Pixels[ GPU64_3D_SURFACE_W * GPU64_3D_SURFACE_H ];
static u16 g_Depth[ GPU64_3D_SURFACE_W * GPU64_3D_SURFACE_H ];
static u8  g_Palette[ 256 * 3 ];

// --- a palette in the shape the design recommends to artists ------------
// Roughly 32 hues x 8 brightnesses, so the colormap has a value range to
// find its darker matches in. This is advice, not a wire-format constraint --
// the point of the colormap is that any palette works, better or worse.
static void buildPalette( void )
{
	static const u8 hue[ 32 ][ 3 ] = {
		{ 255,  64,  64 }, { 255, 112,  48 }, { 255, 160,  32 }, { 255, 208,  32 },
		{ 240, 255,  32 }, { 176, 255,  32 }, { 112, 255,  48 }, {  48, 255,  64 },
		{  32, 255, 128 }, {  32, 255, 192 }, {  32, 240, 255 }, {  32, 192, 255 },
		{  48, 144, 255 }, {  64,  96, 255 }, {  96,  64, 255 }, { 144,  48, 255 },
		{ 192,  32, 255 }, { 240,  32, 255 }, { 255,  32, 192 }, { 255,  48, 128 },
		{ 176, 112,  80 }, { 208, 144, 104 }, { 128,  96,  64 }, {  96, 128,  96 },
		{ 255, 255, 255 }, { 224, 224, 224 }, { 192, 192, 192 }, { 160, 160, 160 },
		{ 128, 128, 128 }, {  96,  96,  96 }, {  64,  64,  64 }, {  32,  32,  32 },
	};

	for ( unsigned h = 0; h < 32; h++ )
		for ( unsigned b = 0; b < 8; b++ )
		{
			const unsigned i = h * 8 + b;
			// b = 7 is the hue at full strength, b = 0 is a near-black
			// version of it.
			for ( unsigned k = 0; k < 3; k++ )
				g_Palette[ i * 3 + k ] = (u8)( (unsigned)hue[ h ][ k ] * ( b + 1 ) / 8 );
		}

	// Index 0 is the background this sim clears to, and index 255 is the
	// firmware's reserved log ink.
	g_Palette[ 0 ] = g_Palette[ 1 ] = g_Palette[ 2 ] = 16;
	g_Palette[ 255 * 3 + 0 ] = g_Palette[ 255 * 3 + 1 ] = g_Palette[ 255 * 3 + 2 ] = 255;
}

// --- a test mesh --------------------------------------------------------
//
// Deliberately not a cube: a box that is wider than it is tall, with the top
// face pushed off-centre, so an upside-down or mirrored render is obvious at
// a glance. A symmetric cube renders identically under half a dozen distinct
// bugs.

#define U( f )	( (s16)( (f) * 256 ) )		// model units -> 8.8

static void putVert( u8 *p, unsigned i, s16 x, s16 y, s16 z )
{
	p[ i * 6 + 0 ] = (u8)( x & 0xff );  p[ i * 6 + 1 ] = (u8)( ( x >> 8 ) & 0xff );
	p[ i * 6 + 2 ] = (u8)( y & 0xff );  p[ i * 6 + 3 ] = (u8)( ( y >> 8 ) & 0xff );
	p[ i * 6 + 4 ] = (u8)( z & 0xff );  p[ i * 6 + 5 ] = (u8)( ( z >> 8 ) & 0xff );
}

static void putFace( u8 *p, unsigned f, u8 a, u8 b, u8 c,
		     u8 ua, u8 va, u8 ub, u8 vb, u8 uc, u8 vc,
		     u8 texid, u8 flags )
{
	u8 *q = p + f * GPU64_3D_FACE_STRIDE;
	q[ 0 ] = a; q[ 1 ] = b; q[ 2 ] = c;
	q[ 3 ] = ua; q[ 4 ] = va;
	q[ 5 ] = ub; q[ 6 ] = vb;
	q[ 7 ] = uc; q[ 8 ] = vc;
	q[ 9 ] = texid; q[ 10 ] = flags; q[ 11 ] = 0;
}

// One quad as two triangles, wound so the normal points out of the box.
static void putQuad( u8 *p, unsigned *pF, u8 a, u8 b, u8 c, u8 d, u8 texid, u8 flags )
{
	const u8 T = 63;
	putFace( p, (*pF)++, a, b, c, 0, 0, T, 0, T, T, texid, flags );
	putFace( p, (*pF)++, a, c, d, 0, 0, T, T, 0, T, texid, flags );
}

static void buildBoxBlobs( u8 *pVerts, u32 *pVertLen, u8 *pFaces, u32 *pFaceLen, u8 texid )
{
	// Half-extents: wide in x, short in y, deep in z. The +y face is shifted
	// towards +x so the silhouette is asymmetric.
	const s16 X = U( 24 ), Y = U( 14 ), Z = U( 20 ), SKEW = U( 7 );

	putVert( pVerts, 0, (s16)-X, (s16)-Y, (s16)-Z );
	putVert( pVerts, 1, (s16) X, (s16)-Y, (s16)-Z );
	putVert( pVerts, 2, (s16) X, (s16)-Y, (s16) Z );
	putVert( pVerts, 3, (s16)-X, (s16)-Y, (s16) Z );
	putVert( pVerts, 4, (s16)( -X + SKEW ), (s16)Y, (s16)-Z );
	putVert( pVerts, 5, (s16)(  X + SKEW ), (s16)Y, (s16)-Z );
	putVert( pVerts, 6, (s16)(  X + SKEW ), (s16)Y, (s16) Z );
	putVert( pVerts, 7, (s16)( -X + SKEW ), (s16)Y, (s16) Z );
	*pVertLen = 8 * GPU64_3D_VERT_STRIDE;

	unsigned f = 0;
	putQuad( pFaces, &f, 0, 1, 2, 3, texid, 0 );			// bottom (-y)
	putQuad( pFaces, &f, 7, 6, 5, 4, texid, 0 );			// top    (+y)
	putQuad( pFaces, &f, 0, 4, 5, 1, texid, 0 );			// front  (-z)
	putQuad( pFaces, &f, 2, 6, 7, 3, texid, 0 );			// back   (+z)
	putQuad( pFaces, &f, 3, 7, 4, 0, texid, 0 );			// left   (-x)
	putQuad( pFaces, &f, 1, 5, 6, 2, texid, 0 );			// right  (+x)
	*pFaceLen = f * GPU64_3D_FACE_STRIDE;
}

// --- a texture ----------------------------------------------------------
// A 64x64 checker with a single off-centre marker square, so a rotated or
// mirrored texture lookup is visible rather than merely suspected.
static void buildCheckerBlob( u8 *p, u8 nA, u8 nB, u8 nMark )
{
	for ( unsigned y = 0; y < 64; y++ )
		for ( unsigned x = 0; x < 64; x++ )
		{
			u8 c = ( ( ( x >> 3 ) ^ ( y >> 3 ) ) & 1 ) ? nA : nB;
			if ( x >= 4 && x < 16 && y >= 4 && y < 10 )
				c = nMark;
			p[ y * 64 + x ] = c;
		}
}

// --- texture table for the lookup callback ------------------------------

struct TexTable
{
	Gpu64_3dTexture	tex[ 4 ];
	boolean		bValid[ 4 ];
};

static const Gpu64_3dTexture *lookupTex( void *pCtx, u16 nId )
{
	TexTable *pT = (TexTable *)pCtx;
	if ( nId < 4 && pT->bValid[ nId ] )
		return &pT->tex[ nId ];
	return 0;
}

// --- camera -------------------------------------------------------------
// A camera is a node with a position and an orientation; the view transform
// is its inverse -- rotate the world by the transpose, after subtracting the
// camera position. Doing it here, in the sim, is what proves the state's
// viewRot/viewPos pair is enough before CREATE_CAMERA exists in firmware.
static void setCamera( Gpu64_3dState *pState, s32 x, s32 y, s32 z,
		       u16 nYaw, u16 nPitch, u16 nRoll )
{
	Gpu64_3dMat cam;
	gpu64_3dMatFromEuler( &cam, nYaw, nPitch, nRoll );
	gpu64_3dMatTranspose( &pState->viewRot, &cam );

	pState->viewPos.x = x;
	pState->viewPos.y = y;
	pState->viewPos.z = z;
	pState->bHaveCamera = TRUE;
}

// --- output -------------------------------------------------------------

static void writePPM( const char *pPath, const Gpu64_3dState *pState )
{
	FILE *f = fopen( pPath, "wb" );
	if ( !f )
	{
		fprintf( stderr, "hostsim: cannot write %s\n", pPath );
		return;
	}

	fprintf( f, "P6\n%d %d\n255\n", GPU64_3D_SURFACE_W, GPU64_3D_SURFACE_H );

	for ( unsigned y = 0; y < GPU64_3D_SURFACE_H; y++ )
		for ( unsigned x = 0; x < GPU64_3D_SURFACE_W; x++ )
		{
			const u8 i = g_Pixels[ y * GPU64_3D_SURFACE_W + x ];
			fwrite( &g_Palette[ i * 3 ], 1, 3, f );
		}

	(void)pState;
	fclose( f );
}

static u32 checksum( void )
{
	u32 h = 2166136261u;
	for ( unsigned i = 0; i < sizeof( g_Pixels ); i++ )
	{
		h ^= g_Pixels[ i ];
		h *= 16777619u;
	}
	return h;
}

static double nowMs( void )
{
	struct timespec ts;
	clock_gettime( CLOCK_MONOTONIC, &ts );
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// --- the run ------------------------------------------------------------

int main( int argc, char **argv )
{
	const char *pOutDir = argc > 1 ? argv[ 1 ] : ".";

	buildPalette();

	Gpu64_3dState state;
	gpu64_3dStateDefaults( &state );

	double t0 = nowMs();
	gpu64_3dBuildColormap( &state, g_Palette );
	printf( "BUILD_COLORMAP: %.2f ms on this host\n", nowMs() - t0 );

	// A 256x160 viewport centred in the 320x200 surface -- the design's
	// reference size, leaving a margin the C64's own class-0 HUD would own.
	state.vpW = 256;
	state.vpH = 160;
	state.vpX = ( GPU64_3D_SURFACE_W - state.vpW ) / 2;
	state.vpY = ( GPU64_3D_SURFACE_H - state.vpH ) / 2;
	state.focal = gpu64_3dFocalFromFov( state.fov, state.vpW );
	state.background = 0;
	state.ambient = 3;

	// Light from the upper left and slightly towards the viewer.
	gpu64_3dNormalise( state.lightDir, -5, 7, -6 );

	// Camera raised and tilted down, so more than one face of the box is in
	// view. A head-on camera sees exactly one face of a box and would hide
	// every bug that lives in the second one.
	// A positive pitch tips a node's own +z towards -y, i.e. a camera with a
	// positive pitch looks *down*. Worth stating: the sign is the kind of
	// convention that is obvious once and mysterious a month later.
	setCamera( &state, 0, 26 * GPU64_FX16_ONE, 0, 0, (u16)( 65536 / 24 ), 0 );

	printf( "viewport %ux%u at (%u,%u), focal %.2f px, budget %u/%u\n",
		state.vpW, state.vpH, state.vpX, state.vpY,
		state.focal / 65536.0, state.vpW * state.vpH * 3, GPU64_3D_BUDGET );

	// --- resources ---
	TexTable tt;
	memset( &tt, 0, sizeof( tt ) );

	u8 checker[ 64 * 64 ];
	buildCheckerBlob( checker, (u8)( 10 * 8 + 7 ), (u8)( 13 * 8 + 4 ), (u8)( 3 * 8 + 7 ) );
	if ( gpu64_3dBuildTexture( &tt.tex[ 1 ], checker, sizeof( checker ), 6, 6, hostAlloc, 0 ) != GPU64_3D_OK )
	{
		fprintf( stderr, "hostsim: texture build failed\n" );
		return 1;
	}
	tt.bValid[ 1 ] = TRUE;

	u8 vertBlob[ 8 * GPU64_3D_VERT_STRIDE ];
	u8 faceBlob[ 12 * GPU64_3D_FACE_STRIDE ];
	u32 vertLen, faceLen;
	buildBoxBlobs( vertBlob, &vertLen, faceBlob, &faceLen, 1 );

	Gpu64_3dMesh mesh;
	u8 res = gpu64_3dBuildMesh( &mesh, vertBlob, vertLen, faceBlob, faceLen, hostAlloc, 0 );
	if ( res != GPU64_3D_OK )
	{
		fprintf( stderr, "hostsim: mesh build failed, err %02x\n", res );
		return 1;
	}
	printf( "mesh: %u verts, %u faces, bounding radius %.2f units\n",
		mesh.nVerts, mesh.nFaces, mesh.radius / 256.0 );

	Gpu64_3dTarget target;
	target.pPixels = g_Pixels;
	target.pitch   = GPU64_3D_SURFACE_W;
	target.pDepth  = g_Depth;

	static Gpu64_3dScratch scratch;

	// --- a turn, eight frames ---
	char path[ 512 ];
	double totalMs = 0;
	unsigned totalTris = 0;

	for ( unsigned frame = 0; frame < 8; frame++ )
	{
		// The box only spins; the elevation comes from the camera. Tilting
		// the model as well was the first thing tried and it cancelled the
		// camera out -- two faces became one, which reads as a culling bug
		// and is not one.
		const u16 yaw = (u16)( frame * ( 65536 / 8 ) );

		Gpu64_3dMat rot;
		gpu64_3dMatFromEuler( &rot, yaw, 0, 0 );

		Gpu64_3dVec pos;
		pos.x = 0;
		pos.y = 0;
		pos.z = 70 * GPU64_FX16_ONE;

		const double t = nowMs();
		gpu64_3dClearViewport( &state, &target );
		const unsigned n = gpu64_3dDrawMesh( &state, &target, &scratch, &mesh,
						     &pos, &rot, 1 * 256, lookupTex, &tt );
		const double ms = nowMs() - t;
		totalMs += ms;
		totalTris += n;

		snprintf( path, sizeof( path ), "%s/frame%02u.ppm", pOutDir, frame );
		writePPM( path, &state );
		printf( "  frame %u: yaw %5u, %2u tris, %.3f ms, checksum %08x\n",
			frame, yaw, n, ms, checksum() );
	}

	printf( "%u triangles over 8 frames, %.3f ms/frame on this host\n",
		totalTris, totalMs / 8 );

	// --- a z-buffer test: two boxes that interpenetrate -----------------
	// Painter's algorithm cannot draw this correctly, which is precisely why
	// the design pays for a z-buffer. If the seam between the two boxes is
	// not a clean diagonal, the depth test is wrong.
	{
		gpu64_3dClearViewport( &state, &target );

		Gpu64_3dMat rot;
		Gpu64_3dVec pos;
		unsigned n = 0;

		gpu64_3dMatFromEuler( &rot, 0, 0, 0 );
		pos.x = -10 * GPU64_FX16_ONE; pos.y = 0; pos.z = 70 * GPU64_FX16_ONE;
		n += gpu64_3dDrawMesh( &state, &target, &scratch, &mesh, &pos, &rot, 256, lookupTex, &tt );

		gpu64_3dMatFromEuler( &rot, 65536 / 8, 65536 / 8, 0 );
		pos.x = 14 * GPU64_FX16_ONE; pos.y = 0; pos.z = 62 * GPU64_FX16_ONE;
		n += gpu64_3dDrawMesh( &state, &target, &scratch, &mesh, &pos, &rot, 256, lookupTex, &tt );

		snprintf( path, sizeof( path ), "%s/zbuffer.ppm", pOutDir );
		writePPM( path, &state );
		printf( "z-buffer test: %u tris, checksum %08x\n", n, checksum() );
	}

	// --- a near-plane clip test -----------------------------------------
	// The box straddles the near plane, so the clipper has to produce quads
	// out of triangles. A hole, a wedge, or geometry wrapping to the far side
	// of the viewport all mean clipNear() is wrong.
	{
		gpu64_3dClearViewport( &state, &target );

		// Straddling needs the box both close and turned: a box centred on
		// the view axis shows only its front face, and that face is either
		// wholly in front of the near plane or wholly behind it -- which
		// clips to nothing and tests nothing. So: camera back at the origin,
		// box pushed off to one side and turned 45 degrees, close enough that
		// its near corner is behind the eye.
		setCamera( &state, 0, 0, 0, 0, 0, 0 );

		Gpu64_3dMat rot;
		gpu64_3dMatFromEuler( &rot, 65536 / 8, 0, 0 );

		Gpu64_3dVec pos;
		pos.x = 22 * GPU64_FX16_ONE; pos.y = 0; pos.z = 14 * GPU64_FX16_ONE;

		const unsigned n = gpu64_3dDrawMesh( &state, &target, &scratch, &mesh,
						     &pos, &rot, 256, lookupTex, &tt );

		snprintf( path, sizeof( path ), "%s/nearclip.ppm", pOutDir );
		writePPM( path, &state );
		printf( "near-clip test: %u tris, checksum %08x\n", n, checksum() );
	}

	// --- flat-colour and unlit paths ------------------------------------
	{
		u8 flatFaces[ 12 * GPU64_3D_FACE_STRIDE ];
		u32 flatLen;
		u8 flatVerts[ 8 * GPU64_3D_VERT_STRIDE ];
		u32 flatVertLen;
		buildBoxBlobs( flatVerts, &flatVertLen, flatFaces, &flatLen, (u8)( 6 * 8 + 7 ) );
		for ( unsigned f = 0; f < flatLen / GPU64_3D_FACE_STRIDE; f++ )
			flatFaces[ f * GPU64_3D_FACE_STRIDE + 10 ] = GPU64_3D_FACE_FLAT_COLOUR;

		Gpu64_3dMesh flatMesh;
		if ( gpu64_3dBuildMesh( &flatMesh, flatVerts, flatVertLen, flatFaces, flatLen,
					hostAlloc, 0 ) != GPU64_3D_OK )
		{
			fprintf( stderr, "hostsim: flat mesh build failed\n" );
			return 1;
		}

		gpu64_3dClearViewport( &state, &target );

		Gpu64_3dMat rot;
		gpu64_3dMatFromEuler( &rot, 65536 / 10, 65536 / 14, 65536 / 40 );

		Gpu64_3dVec pos;
		pos.x = 0; pos.y = 0; pos.z = 70 * GPU64_FX16_ONE;

		const unsigned n = gpu64_3dDrawMesh( &state, &target, &scratch, &flatMesh,
						     &pos, &rot, 256, lookupTex, &tt );

		snprintf( path, sizeof( path ), "%s/flatcolour.ppm", pOutDir );
		writePPM( path, &state );
		printf( "flat-colour test: %u tris, checksum %08x\n", n, checksum() );
	}

	// --- exactly what Source/TestPRG/gpu64_3d_cube.a asks for -----------
	// The C64-side demo has no camera opcode -- CREATE_CAMERA is phase 2 --
	// so its view is the identity at the origin and the elevation comes from
	// a negative model pitch. Rendering that same setup here gives the bench
	// a reference picture to hold the HDMI output against, which is the
	// difference between "that looks wrong" and "that looks wrong *here*".
	{
		setCamera( &state, 0, 0, 0, 0, 0, 0 );
		state.ambient = 3;

		gpu64_3dClearViewport( &state, &target );

		Gpu64_3dMat rot;
		gpu64_3dMatFromEuler( &rot, 0x2000, 0xF000, 0 );

		Gpu64_3dVec pos;
		pos.x = 0; pos.y = 0; pos.z = 70 * GPU64_FX16_ONE;

		const unsigned n = gpu64_3dDrawMesh( &state, &target, &scratch, &mesh,
						     &pos, &rot, 256, lookupTex, &tt );

		snprintf( path, sizeof( path ), "%s/prgpreview.ppm", pOutDir );
		writePPM( path, &state );
		printf( "PRG preview: %u tris, checksum %08x\n", n, checksum() );
	}

	// --- the scene graph: stage 14 ---------------------------------------
	// Exercises every opcode in the vertical slice (CREATE_OBJECT/CAMERA,
	// SET_POSITION/ORIENTATION, MOVE_LOCAL/WORLD, ROTATE_LOCAL, SET_SCALE,
	// SET_ACTIVE_CAMERA, SET_VISIBLE, DESTROY_NODE, GET_TRANSFORM) plus the
	// DRAW_NODE draw path -- gpu64_3d_class1.cpp's opDrawNode() does exactly
	// this sequence (resolve node, check visible, resolve mesh via the ID the
	// node carries, call gpu64_3dDrawMesh() with the node's own pos/rot/scale)
	// except for the resource-ID lookup, which needs the C64 bus and so has
	// no counterpart here -- `mesh` stands in for whatever CREATE_OBJECT's
	// meshId would have resolved to.
	{
		int failures = 0;
#define CHECK( label, got, want ) \
		do { if ( (got) != (want) ) { \
			fprintf( stderr, "hostsim: SCENE %s: got %02x, want %02x\n", \
				 label, (unsigned)(got), (unsigned)(want) ); \
			failures++; \
		} } while ( 0 )

		Gpu64_3dScene scene;
		gpu64_3dSceneReset( &scene );

		const u16 kObjId = 1, kCamId = 2;

		CHECK( "CREATE_OBJECT", gpu64_3dSceneCreateObject( &scene, kObjId, 42 ), GPU64_3D_OK );
		CHECK( "CREATE_CAMERA", gpu64_3dSceneCreateCamera( &scene, kCamId ), GPU64_3D_OK );

		Gpu64_3dVec objPos; objPos.x = 0; objPos.y = 0; objPos.z = 70 * GPU64_FX16_ONE;
		CHECK( "SET_POSITION(obj)", gpu64_3dSceneSetPosition( &scene, kObjId, &objPos ), GPU64_3D_OK );
		CHECK( "SET_ORIENTATION(obj)",
		       gpu64_3dSceneSetOrientation( &scene, kObjId, (u16)( 65536 / 8 ), 0, 0 ), GPU64_3D_OK );

		// MOVE_LOCAL walks along the node's own +z, which SET_ORIENTATION
		// just turned 45 degrees off the world +z -- so this must not land on
		// the world-z-only path MOVE_WORLD would take.
		CHECK( "MOVE_LOCAL", gpu64_3dSceneMoveLocal( &scene, kObjId, 0, 0, 5 * GPU64_FX16_ONE ), GPU64_3D_OK );
		CHECK( "MOVE_WORLD", gpu64_3dSceneMoveWorld( &scene, kObjId, 2 * GPU64_FX16_ONE, 0, 0 ), GPU64_3D_OK );
		CHECK( "ROTATE_LOCAL",
		       gpu64_3dSceneRotateLocal( &scene, kObjId, (u16)( 65536 / 16 ), 0, 0 ), GPU64_3D_OK );
		CHECK( "SET_SCALE", gpu64_3dSceneSetScale( &scene, kObjId, (u16)( 1.25 * GPU64_FX8_ONE ) ), GPU64_3D_OK );
		CHECK( "SET_SCALE(0)", gpu64_3dSceneSetScale( &scene, kObjId, 0 ), GPU64_3D_BAD_ARGS );

		// Camera raised and tilted down -- same pose setCamera() above has
		// been using by hand, now driven through CREATE_CAMERA + the
		// transform opcodes instead.
		Gpu64_3dVec camPos; camPos.x = 0; camPos.y = 26 * GPU64_FX16_ONE; camPos.z = 0;
		CHECK( "SET_POSITION(cam)", gpu64_3dSceneSetPosition( &scene, kCamId, &camPos ), GPU64_3D_OK );
		CHECK( "SET_ORIENTATION(cam)",
		       gpu64_3dSceneSetOrientation( &scene, kCamId, 0, (u16)( 65536 / 24 ), 0 ), GPU64_3D_OK );
		CHECK( "SET_ACTIVE_CAMERA", gpu64_3dSceneSetActiveCamera( &scene, kCamId ), GPU64_3D_OK );
		CHECK( "SET_ACTIVE_CAMERA(bad id)", gpu64_3dSceneSetActiveCamera( &scene, 999 ), GPU64_3D_BAD_ID );

		gpu64_3dSceneApplyCamera( &scene, &state );
		if ( !state.bHaveCamera )
		{
			fprintf( stderr, "hostsim: SCENE ApplyCamera left bHaveCamera FALSE\n" );
			failures++;
		}

		Gpu64_3dVec gotPos; u16 gotYaw, gotPitch, gotRoll;
		CHECK( "GET_TRANSFORM", gpu64_3dSceneGetTransform( &scene, kObjId, &gotPos, &gotYaw, &gotPitch, &gotRoll ),
		       GPU64_3D_OK );
		// yaw was set to 65536/8 then advanced by ROTATE_LOCAL's 65536/16 --
		// GET_TRANSFORM must read back the accumulated angle, not the one
		// SET_ORIENTATION last wrote.
		CHECK( "GET_TRANSFORM yaw", gotYaw, (u16)( 65536 / 8 + 65536 / 16 ) );

		Gpu64_3dNode *pObj = gpu64_3dSceneFind( &scene, kObjId, GPU64_3D_NODE_OBJECT );
		if ( pObj == 0 )
		{
			fprintf( stderr, "hostsim: SCENE lost the object node\n" );
			failures++;
		}
		else
		{
			gpu64_3dClearViewport( &state, &target );
			const unsigned n = gpu64_3dDrawMesh( &state, &target, &scratch, &mesh,
							     &pObj->pos, &pObj->rot, pObj->scale, lookupTex, &tt );
			snprintf( path, sizeof( path ), "%s/scenegraph.ppm", pOutDir );
			writePPM( path, &state );
			printf( "scene graph test: %u tris, checksum %08x\n", n, checksum() );

			// SET_VISIBLE(0) must be honoured by the DRAW_NODE path -- here,
			// by whatever reads pObj->visible the way opDrawNode() does.
			CHECK( "SET_VISIBLE(0)", gpu64_3dSceneSetVisible( &scene, kObjId, FALSE ), GPU64_3D_OK );
			if ( pObj->visible )
			{
				fprintf( stderr, "hostsim: SCENE SET_VISIBLE(0) left the node visible\n" );
				failures++;
			}
		}

		// Destroying the active camera must clear bHaveActiveCamera, not
		// leave a dangling ID a later CREATE_OBJECT could silently inherit --
		// see gpu64_3dSceneDestroyNode()'s own comment.
		CHECK( "DESTROY_NODE(cam)", gpu64_3dSceneDestroyNode( &scene, kCamId ), GPU64_3D_OK );
		gpu64_3dSceneApplyCamera( &scene, &state );
		if ( state.bHaveCamera )
		{
			fprintf( stderr, "hostsim: SCENE ApplyCamera kept a camera after DESTROY_NODE\n" );
			failures++;
		}
		CHECK( "DESTROY_NODE(already gone)", gpu64_3dSceneDestroyNode( &scene, kCamId ), GPU64_3D_BAD_ID );

		// The 257th node must be refused: GPU64_3D_MAX_NODES is the design
		// doc's own "256 object instances" limit, and nothing upstream of
		// gpu64_3dSceneCreateObject() enforces it -- this is the only test of
		// that ceiling that exists anywhere in the pipeline.
		gpu64_3dSceneReset( &scene );
		u8 fillRes = GPU64_3D_OK;
		for ( unsigned i = 0; i < GPU64_3D_MAX_NODES; i++ )
			fillRes = gpu64_3dSceneCreateObject( &scene, (u16)i, 0 );
		CHECK( "fill to capacity", fillRes, GPU64_3D_OK );
		CHECK( "one past capacity", gpu64_3dSceneCreateObject( &scene, 0xffff, 0 ), GPU64_3D_OUT_OF_MEMORY );

#undef CHECK
		if ( failures )
		{
			fprintf( stderr, "hostsim: %d scene graph check(s) failed\n", failures );
			return 1;
		}
		printf( "scene graph: all checks passed\n" );
	}

	printf( "PPMs written to %s/\n", pOutDir );
	return 0;
}
