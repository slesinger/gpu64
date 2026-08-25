/*
 gpu64: IO2 command API register file, dispatcher and class-0 opcodes.
 See gpu64_api.h and docs/api_design.md.
*/
#include "gpu64_api.h"
#include "gpu64_fb.h"
#include "gpu64_vsync.h"
#include "gpu64_flip.h"
#include "gpu64_ladder.h"
#include "gpu64_3d.h"
#include "gpu64_raster.h"
#include <circle/util.h>

// gpu64: set once a program actually drives the API, which stops the
// milestone 3 screen mirror (the two modes are mutually exclusive). Declared
// in rad_reu.cpp, which also clears it on every fresh REU session.
extern u8 gpu64ApiActive;

// --- register file ------------------------------------------------------
// Global, and reached from the polling loop without a call -- see the
// comment on GPU64REGS in gpu64_api.h for why. The short aliases keep the
// rest of this file reading the way it did.
GPU64REGS gpu64Regs = { 0, 0, GPU64_ERR_OK, { 0, 0 }, 0, { 0 } };

#define sCmdHi	gpu64Regs.cmdHi
#define sStatus	gpu64Regs.status
#define sErr	gpu64Regs.err
#define sId		gpu64Regs.id
#define sResult	gpu64Regs.result
#define sArg	gpu64Regs.arg

// gpu64: payload staging. Static rather than stack-local -- these are far too
// big for reuUsingPolling()'s stack, and the dispatcher is called from inside
// its loop.
static u8 sBlobA[ 65536 ];
static u8 sBlobB[ 65536 ];
static u8 sBlobC[ 65536 ];

// Matrix inversion works in double precision regardless of the operand
// format (see MAT_INVERSE below), on an n x 2n augmented matrix.
#define GPU64_MAX_INVERSE_N	64
static double sInvWork[ GPU64_MAX_INVERSE_N * GPU64_MAX_INVERSE_N * 2 ];

void gpu64_apiReset( void )
{
	sCmdHi  = 0;
	sStatus = 0;
	sErr    = GPU64_ERR_OK;
	sId[ 0 ] = sId[ 1 ] = 0;
	sResult = 0;
	for ( unsigned i = 0; i < GPU64_ARG_COUNT; i++ )
		sArg[ i ] = 0;
	// The frame clock's calibration survives -- it describes the display,
	// not the session -- but nothing else about the vblank state does.
	gpu64_vsyncResetState();

	// Class 1's session state goes the same way: ring emptied, every
	// uploaded resource freed. See docs/milestone6_3d_design.md's Resource
	// lifecycle -- without this a RUN/STOP+RESTORE leaks the whole arena and
	// the next program starts against stale IDs.
#ifdef GPU64_3D_ENABLED
	gpu64_3dReset();
#endif

	// Class 2 goes the same way: every texture freed, the view back to the
	// full surface, lighting back to identity.
#ifdef GPU64_RASTER_ENABLED
	gpu64_rasterReset();
#endif
}

// --- argument accessors -------------------------------------------------
static inline u16 argU16( unsigned i )
{
	return (u16)( sArg[ i ] | ( sArg[ i + 1 ] << 8 ) );
}

static inline int argS16( unsigned i )
{
	return (int)(s16)argU16( i );
}

// Full 6-byte blob descriptor: space, 24-bit addr, 16-bit len.
static inline void argBlob( unsigned i, u8 *pSpace, u32 *pAddr, u32 *pLen )
{
	*pSpace = sArg[ i ];
	*pAddr  = (u32)sArg[ i + 1 ] | ( (u32)sArg[ i + 2 ] << 8 ) | ( (u32)sArg[ i + 3 ] << 16 );
	*pLen   = (u32)sArg[ i + 4 ] | ( (u32)sArg[ i + 5 ] << 8 );
}

// 4-byte compact descriptor (math operands): space + 24-bit addr, no length
// -- dimensions and element size imply it.
static inline void argCompact( unsigned i, u8 *pSpace, u32 *pAddr )
{
	*pSpace = sArg[ i ];
	*pAddr  = (u32)sArg[ i + 1 ] | ( (u32)sArg[ i + 2 ] << 8 ) | ( (u32)sArg[ i + 3 ] << 16 );
}

// --- fixed-point helpers ------------------------------------------------
//
// Elements are signed 16-bit 8.8. Products accumulate at full width (s64,
// since a 255-term dot product of 16.16 values overflows s32), then come back
// to 8.8 by rounding half away from zero and saturating rather than wrapping
// -- a saturated highlight looks like clipping, a wrapped one looks like a
// hardware fault. Documented in docs/api_design.md because C64 code can see
// the difference.
static inline s16 fixRead( const u8 *p, u32 i )
{
	return (s16)( p[ 2 * i ] | ( p[ 2 * i + 1 ] << 8 ) );
}

// gpu64: rounds half away from zero, which is what docs/api_design.md
// specifies. The bias has to be paired with a division that truncates
// toward zero, NOT with an arithmetic shift: >> floors, so on a negative
// accumulator the -128 bias is applied a second time and every negative
// 8.8 product came back one LSB further from zero than it should --
// -1.0 * 1.0 returned $FEFF (-1.00391) instead of $FF00. Positive products
// were never affected, which is why this survived a hardware-verified
// milestone: a one-LSB error on negatives only is invisible in a rendered
// frame. Found by gpu64_test_math, predicted on a PC and confirmed on
// hardware as exactly three red lines (ROUND NEG, MUL MINUS ONE,
// SCALE NEG) and no others.
static inline void fixWrite( u8 *p, u32 i, s64 acc )
{
	acc = ( acc >= 0 ) ? ( acc + 128 ) / 256 : -( ( -acc + 128 ) / 256 );
	if ( acc >  32767 ) acc =  32767;
	if ( acc < -32768 ) acc = -32768;
	p[ 2 * i ]     = (u8)( (u16)acc & 0xff );
	p[ 2 * i + 1 ] = (u8)( ( (u16)acc >> 8 ) & 0xff );
}

// Same saturation, for values that are already 8.8 (add/sub/transpose).
static inline void fixStore( u8 *p, u32 i, s32 v )
{
	if ( v >  32767 ) v =  32767;
	if ( v < -32768 ) v = -32768;
	p[ 2 * i ]     = (u8)( (u16)v & 0xff );
	p[ 2 * i + 1 ] = (u8)( ( (u16)v >> 8 ) & 0xff );
}

static inline float fltRead( const u8 *p, u32 i )
{
	float f;
	memcpy( &f, p + 4 * i, 4 );
	return f;
}

static inline void fltWrite( u8 *p, u32 i, float f )
{
	memcpy( p + 4 * i, &f, 4 );
}

// --- class 0 dispatch ---------------------------------------------------

// gpu64: LOG_ENABLE(1) is the only command exempt from the log auto-hide, so
// it is the one place a bench program can ask the firmware to say something
// back. Milestone 4c's flip rework exists to move a measured cost, so it
// reports the measurement -- otherwise the only way to know whether the
// mailbox split actually helped is to believe it did. See gpu64_flip.h.
static char *logDec( char *p, u32 v )
{
	char tmp[ 10 ];
	unsigned n = 0;
	do { tmp[ n++ ] = (char)( '0' + v % 10 ); v /= 10; } while ( v != 0 );
	while ( n-- ) *p++ = tmp[ n ];
	return p;
}

static char *logTriple( char *p, u32 nMin, u32 nTotal, u32 nMax, u32 nCount )
{
	if ( nCount == 0 )
		{ *p++ = '-'; return p; }
	p = logDec( p, nMin );  *p++ = '/';
	p = logDec( p, nTotal / nCount ); *p++ = '/';
	p = logDec( p, nMax );  *p++ = 'u'; *p++ = 's';
	return p;
}

static void logFlipStats( CGpu64FrameBuffer *pFB )
{
	if ( pFB == 0 )
		return;

	// Two lines, and every field is a u32 -- sized for the absurd case
	// (ten digits everywhere) rather than the realistic one, because this
	// writes without bounds checking.
	char line[ 192 ];
	char *p = line;

	const char *pTag = gpu64_flipAvailable() ? "FLIP fast n=" : "FLIP SLOW n=";
	for ( const char *q = pTag; *q; q++ ) *p++ = *q;
	p = logDec( p, gpu64FlipStats.postCount );

	*p++ = ' '; *p++ = 'h'; *p++ = '=';
	p = logTriple( p, gpu64FlipStats.postMinUs, gpu64FlipStats.postTotalUs,
			  gpu64FlipStats.postMaxUs, gpu64FlipStats.postCount );
	*p++ = '\n';

	for ( const char *q = "FLIP wait n="; *q; q++ ) *p++ = *q;
	p = logDec( p, gpu64FlipStats.drainCount );
	*p++ = ' ';
	p = logTriple( p, gpu64FlipStats.drainMinUs, gpu64FlipStats.drainTotalUs,
			  gpu64FlipStats.drainMaxUs, gpu64FlipStats.drainCount );
	*p++ = ' '; *p++ = 's'; *p++ = '=';
	p = logDec( p, gpu64FlipStats.slowCount );
	*p++ = '\n';

	pFB->LogWrite( line, (unsigned)( p - line ) );
}

// gpu64: milestone 6's load-ladder table rides the same reporting hook as
// the flip stats -- LOG_ENABLE(1) is still the one command a bench program
// has for asking the firmware to say something back. Compiles away in a
// normal build (gpu64_ladder.h).
static void logLadderStats( CGpu64FrameBuffer *pFB )
{
#ifdef GPU64_LADDER_ENABLED
	if ( pFB == 0 )
		return;

	// One header line plus one line per rung, each well inside the log's 40
	// columns; sized generously because gpu64_ladderReport() writes without
	// bounds checking, exactly like logFlipStats() above. Static, per the
	// note on the blob buffers at the top of this file: this runs on
	// reuUsingPolling()'s stack.
	static char line[ 64 * ( GPU64_LADDER_RUNGS + 2 ) ];
	char *p = gpu64_ladderReport( line );

	pFB->LogWrite( line, (unsigned)( p - line ) );
#else
	(void)pFB;
#endif
}

// gpu64: milestone 6's class 1 counters ride the same LOG_ENABLE(1) hook as
// the flip stats and the ladder table. Reading gpu64_3dWorkerStats here means
// core 0 touches a line core 1 writes -- normally forbidden (milestone 6a
// rounds 8-10) -- but this runs only from the log path, never from
// reuUsingPolling()'s PHI-locked hot loop, so the coherence miss has nowhere
// to make core 0 late. Do not call it from anywhere else.
static void logGpu64_3dStats( CGpu64FrameBuffer *pFB )
{
#ifdef GPU64_3D_ENABLED
	if ( pFB == 0 )
		return;

	// One line, well inside the log's 40 columns. Static for the same reason
	// the blob buffers at the top of this file are: this runs on
	// reuUsingPolling()'s stack.
	static char line[ 96 ];
	char *p = gpu64_3dReport( line );
	*p++ = '\n';

	pFB->LogWrite( line, (unsigned)( p - line ) );
#else
	(void)pFB;
#endif
}

// gpu64: class 2's counters on the same LOG_ENABLE(1) hook. What a bench
// round reads here is `rej=` -- a batch that lost bytes on the way across the
// bus rejects records that should have been accepted, which is the
// length-readback half of CLAUDE.md's rule 3.
static void logGpu64_RasterStats( CGpu64FrameBuffer *pFB )
{
#ifdef GPU64_RASTER_ENABLED
	if ( pFB == 0 )
		return;

	// One line, inside the log's 40 columns. Static for the same reason the
	// blob buffers at the top of this file are.
	static char line[ 96 ];
	char *p = gpu64_rasterReport( line );
	*p++ = '\n';

	pFB->LogWrite( line, (unsigned)( p - line ) );
#else
	(void)pFB;
#endif
}

// gpu64: the crash-path escape hatch, added after round 2 (2026-08-23).
//
// Both ladder rounds so far ended with the C64 derailed and back in the RAD
// menu, which means the test PRG never reached its closing LOG_ENABLE(1)
// and the whole table was lost -- the run cost a reflash and told us
// nothing quantitative. A run that fails is exactly the run whose numbers
// matter most, so the loop now dumps the table on its way out instead.
//
// Forces the log back on first: the auto-hide turned it off at the first
// command of the session, and DrawLogOverlay() is a no-op while it is off.
void gpu64_ladderDumpNow( void )
{
#ifdef GPU64_LADDER_ENABLED
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	if ( pFB == 0 )
		return;

	pFB->LogEnable( TRUE );
	logFlipStats( pFB );
	logLadderStats( pFB );
	logGpu64_3dStats( pFB );
	logGpu64_RasterStats( pFB );
#endif
}

static u8 doSystem( u8 op )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;

	switch ( op )
	{
	case 0x00:					// NOP
		return GPU64_ERR_OK;

	case 0x01:					// RESET_STATE
		sStatus = 0;
		gpu64_vsyncResetState();
		if ( pFB ) pFB->ResetPages();
		return GPU64_ERR_OK;

	case 0x02:					// VBLANK_ARM
		if ( sArg[ 0 ] > 1 )
			return GPU64_ERR_BAD_ARGS;
		if ( sArg[ 0 ] == 1 )
		{
			if ( !gpu64Vsync.calibrated )
				return GPU64_ERR_UNSUPPORTED;
			// Arming is a setup-time command, so it is the natural place to
			// pay for a re-anchor: the extrapolated clock is at its most
			// accurate right after one, and the up-to-one-frame halt this
			// costs happens once rather than per frame. See gpu64_vsync.h.
			gpu64_vsyncReanchor();
			gpu64Vsync.armed = 1;
			sStatus |= GPU64_STATUS_VBLANK_ARMED;
		} else
		{
			gpu64Vsync.armed = 0;
			sStatus &= ~GPU64_STATUS_VBLANK_ARMED;
			// Disarming while the line is still held would leave the C64
			// with an IRQ it can no longer explain.
			if ( gpu64Vsync.irqAsserted )
				gpu64Vsync.irqReleaseReq = 1;
			gpu64Vsync.irqRequest = 0;
		}
		return GPU64_ERR_OK;

	case 0x03:					// VBLANK_ACK
		sStatus &= ~GPU64_STATUS_VBLANK_PENDING;
		// Releasing bIRQ_OUT is the bus-watch loop's job -- it owns every
		// GPIO in the cartridge and is the only place that knows whether
		// REU is holding the same line. This just asks.
		if ( gpu64Vsync.irqAsserted )
			gpu64Vsync.irqReleaseReq = 1;
		return GPU64_ERR_OK;

	case 0x04:					// SET_DRAW_PAGE
		if ( sArg[ 0 ] > 1 )
			return GPU64_ERR_BAD_ARGS;
		if ( pFB ) pFB->SetDrawPage( sArg[ 0 ] );
		return GPU64_ERR_OK;

	case 0x05:					// PAGE_FLIP
		if ( sArg[ 0 ] > 1 )
			return GPU64_ERR_BAD_ARGS;
		if ( pFB == 0 )
			return GPU64_ERR_UNSUPPORTED;
		if ( sArg[ 0 ] == 1 )
		{
			if ( !gpu64Vsync.calibrated )
				return GPU64_ERR_UNSUPPORTED;
			// One flip can be outstanding at a time. Asking for a second
			// changes nothing, per the spec's "a failed dispatch does
			// nothing" rule -- the queued one still lands on its own
			// boundary. The C64 polls STATUS bit0 to know when.
			if ( gpu64Vsync.flipPending )
				return GPU64_ERR_BUSY;
			// Everything expensive about a flip happens now, while the C64
			// is halted for this dispatch anyway; the loop is then left
			// with only the SetVirtualOffset to do at the boundary.
			pFB->PrepareFlip();
			// Warm the commit path now, while the bus is held for this
			// dispatch -- see gpu64_vsyncWarmCommit() in rad_reu.cpp for
			// why it cannot be done at the point of use.
			gpu64_vsyncWarmCommit();
			gpu64Vsync.flipPending = 1;
			sStatus |= GPU64_STATUS_BUSY;
			return GPU64_ERR_OK;
		}
		pFB->Flip();
		return GPU64_ERR_OK;

	case 0x06:					// GET_INFO
	{
		u8 space; u32 addr, len;
		argBlob( 0, &space, &addr, &len );
		if ( len < 16 )
			return GPU64_ERR_BAD_ARGS;

		u8 info[ 16 ];
		memset( info, 0, sizeof info );
		info[ 0 ] = 'G'; info[ 1 ] = '6'; info[ 2 ] = '4';
		info[ 3 ] = 1;					// version major
		info[ 4 ] = 0;					// version minor
		info[ 5 ] = (u8)( GPU64_FB_WIDTH & 0xff );
		info[ 6 ] = (u8)( GPU64_FB_WIDTH >> 8 );
		info[ 7 ] = (u8)( GPU64_FB_HEIGHT & 0xff );
		info[ 8 ] = (u8)( GPU64_FB_HEIGHT >> 8 );
		info[ 9 ] = 8;					// bits per pixel
		info[ 10 ] = GPU64_FB_PAGES;
		// Bitmap of implemented classes, built from the build's own
		// toggles rather than written out by hand -- this byte is how a
		// program discovers class 2 exists, and a hardcoded literal here
		// had already gone stale once (it still said "class 1 not" with
		// class 1 shipping).
		info[ 11 ] = 0x01;				// class 0
#ifdef GPU64_3D_ENABLED
		info[ 11 ] |= 0x02;				// class 1, the 3D pipeline
#endif
#ifdef GPU64_RASTER_ENABLED
		info[ 11 ] |= 0x04;				// class 2, the raster layer
#endif
		info[ 12 ] = GPU64_BORDER_W;			// border width, each side
		info[ 13 ] = GPU64_BORDER_H;			// border height, each side
		// Measured frame period in microseconds, 0 if the frame clock could
		// not be calibrated -- which is also how a program can tell in
		// advance that every vblank feature will return UNSUPPORTED.
		info[ 14 ] = (u8)( gpu64Vsync.periodUs & 0xff );
		info[ 15 ] = (u8)( ( gpu64Vsync.periodUs >> 8 ) & 0xff );
		return gpu64_blobWrite( space, addr, 16, info );
	}

	case 0x08:					// SET_BORDER
		if ( pFB ) pFB->SetBorder( sArg[ 0 ] );
		return GPU64_ERR_OK;

	case 0x07:					// LOG_ENABLE
		if ( sArg[ 0 ] > 1 )
			return GPU64_ERR_BAD_ARGS;
		if ( pFB ) pFB->LogEnable( sArg[ 0 ] ? TRUE : FALSE );
		// Turning the log *on* is a request to be told something, so this is
		// where the flip cost gets reported (logFlipStats() above).
		if ( sArg[ 0 ] ) { logFlipStats( pFB ); logLadderStats( pFB ); logGpu64_3dStats( pFB ); logGpu64_RasterStats( pFB ); }
		return GPU64_ERR_OK;

	case 0x09:					// VBLANK_SYNC
		// The escape hatch for a long-running program: the frame clock is an
		// extrapolation and drifts over minutes (gpu64_vsync.h), so this
		// re-pins it to a real vsync. Costs up to one frame of C64 halt, so
		// it is an occasional housekeeping call, not a per-frame one.
		if ( !gpu64Vsync.calibrated )
			return GPU64_ERR_UNSUPPORTED;
		if ( gpu64Vsync.flipPending )
			return GPU64_ERR_BUSY;
		return gpu64_vsyncReanchor() ? GPU64_ERR_OK : GPU64_ERR_UNSUPPORTED;
	}

	return GPU64_ERR_BAD_OPCODE;
}

static u8 doDraw( u8 op )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	if ( pFB == 0 )
		return GPU64_ERR_UNSUPPORTED;

	switch ( op )
	{
	case 0x10:					// CLEAR
		pFB->Clear( sArg[ 0 ] );
		return GPU64_ERR_OK;

	case 0x20:					// SET_PIXEL
		pFB->SetPixel( argS16( 0 ), argS16( 2 ), sArg[ 4 ] );
		return GPU64_ERR_OK;

	case 0x21:					// LINE
		pFB->Line( argS16( 0 ), argS16( 2 ), argS16( 4 ), argS16( 6 ), sArg[ 8 ] );
		return GPU64_ERR_OK;

	case 0x22:					// RECT
		pFB->Rect( argS16( 0 ), argS16( 2 ), (int)argU16( 4 ), (int)argU16( 6 ), sArg[ 8 ] );
		return GPU64_ERR_OK;

	case 0x23:					// RECT_FILL
		pFB->RectFill( argS16( 0 ), argS16( 2 ), (int)argU16( 4 ), (int)argU16( 6 ), sArg[ 8 ] );
		return GPU64_ERR_OK;

	case 0x30:					// PAL_SET
		pFB->SetPaletteEntry( sArg[ 0 ], sArg[ 1 ], sArg[ 2 ], sArg[ 3 ] );
		pFB->CommitPalette();
		return GPU64_ERR_OK;

	case 0x31:					// PAL_LOAD
	{
		u8 space; u32 addr, len;
		argBlob( 0, &space, &addr, &len );
		u8 first = sArg[ 6 ];
		u8 count = sArg[ 7 ];

		if ( count == 0 )
			return GPU64_ERR_OK;			// no-op, not an error
		if ( len != (u32)count * 3 )
			return GPU64_ERR_BAD_ARGS;
		if ( (u32)first + count > 256 )
			return GPU64_ERR_BAD_ARGS;

		u8 res = gpu64_blobRead( space, addr, len, sBlobA );
		if ( res != GPU64_ERR_OK )
			return res;

		for ( u32 i = 0; i < count; i++ )
			pFB->SetPaletteEntry( (u8)( first + i ), sBlobA[ i * 3 ], sBlobA[ i * 3 + 1 ], sBlobA[ i * 3 + 2 ] );
		pFB->CommitPalette();
		return GPU64_ERR_OK;
	}

	case 0x40:					// BLIT
	case 0x41:					// BLIT_KEYED
	{
		u8 space; u32 addr, len;
		argBlob( 0, &space, &addr, &len );
		int dstX = argS16( 6 );
		int dstY = argS16( 8 );
		u32 w = argU16( 10 );
		u32 h = argU16( 12 );

		if ( w == 0 || h == 0 )
			return GPU64_ERR_OK;
		if ( len != w * h )
			return GPU64_ERR_BAD_ARGS;

		u8 res = gpu64_blobRead( space, addr, len, sBlobA );
		if ( res != GPU64_ERR_OK )
			return res;

		pFB->Blit( sBlobA, dstX, dstY, w, h, ( op == 0x41 ) ? (int)sArg[ 14 ] : -1 );
		return GPU64_ERR_OK;
	}

	case 0x42:					// READ_RECT
	{
		u8 space; u32 addr, len;
		argBlob( 0, &space, &addr, &len );
		int srcX = argS16( 6 );
		int srcY = argS16( 8 );
		u32 w = argU16( 10 );
		u32 h = argU16( 12 );

		if ( w == 0 || h == 0 )
			return GPU64_ERR_OK;
		if ( len != w * h )
			return GPU64_ERR_BAD_ARGS;
		// Reading back is specified as an error, not a clip: a partly
		// offscreen read has no defined content to return.
		if ( srcX < 0 || srcY < 0 ||
		     srcX + (int)w > GPU64_FB_WIDTH || srcY + (int)h > GPU64_FB_HEIGHT )
			return GPU64_ERR_BAD_ARGS;

		pFB->ReadRect( sBlobA, (unsigned)srcX, (unsigned)srcY, w, h );
		return gpu64_blobWrite( space, addr, len, sBlobA );
	}
	}

	return GPU64_ERR_BAD_OPCODE;
}

// --- math ---------------------------------------------------------------

// Byte count of an m x n matrix, or 0 if it does not fit a blob.
static u32 matBytes( u32 rows, u32 cols, u32 elemSize )
{
	u32 n = rows * cols * elemSize;
	if ( n == 0 || n > 65535 )
		return 0;
	return n;
}

static u8 matInverse( u32 n, u32 elemSize, const u8 *pA, u8 *pC )
{
	if ( n > GPU64_MAX_INVERSE_N )
		return GPU64_ERR_BAD_ARGS;

	// Gauss-Jordan on an augmented [A | I], in double precision whatever the
	// operand format is: an 8.8 pivot divides to nothing very quickly, and
	// doing the elimination in the operand's own format would lose the
	// result long before the matrix is actually singular.
	double *w = sInvWork;
	const u32 stride = 2 * n;

	for ( u32 i = 0; i < n; i++ )
		for ( u32 j = 0; j < n; j++ )
		{
			double v = ( elemSize == 2 ) ? (double)fixRead( pA, i * n + j ) / 256.0
						     : (double)fltRead( pA, i * n + j );
			w[ i * stride + j ] = v;
			w[ i * stride + n + j ] = ( i == j ) ? 1.0 : 0.0;
		}

	for ( u32 col = 0; col < n; col++ )
	{
		u32 pivot = col;
		double best = w[ col * stride + col ];
		if ( best < 0 ) best = -best;

		for ( u32 r = col + 1; r < n; r++ )
		{
			double v = w[ r * stride + col ];
			if ( v < 0 ) v = -v;
			if ( v > best ) { best = v; pivot = r; }
		}

		if ( best < 1e-9 )
			return GPU64_ERR_SINGULAR;

		if ( pivot != col )
			for ( u32 j = 0; j < stride; j++ )
			{
				double t = w[ col * stride + j ];
				w[ col * stride + j ] = w[ pivot * stride + j ];
				w[ pivot * stride + j ] = t;
			}

		double d = w[ col * stride + col ];
		for ( u32 j = 0; j < stride; j++ )
			w[ col * stride + j ] /= d;

		for ( u32 r = 0; r < n; r++ )
		{
			if ( r == col )
				continue;
			double f = w[ r * stride + col ];
			if ( f == 0.0 )
				continue;
			for ( u32 j = 0; j < stride; j++ )
				w[ r * stride + j ] -= f * w[ col * stride + j ];
		}
	}

	for ( u32 i = 0; i < n; i++ )
		for ( u32 j = 0; j < n; j++ )
		{
			double v = w[ i * stride + n + j ];
			if ( elemSize == 2 )
				fixWrite( pC, i * n + j, (s64)( v * 65536.0 ) );	// v<<16, then >>8 in fixWrite
			else
				fltWrite( pC, i * n + j, (float)v );
		}

	return GPU64_ERR_OK;
}

static u8 doMath( u8 op )
{
	// Low nibble picks the operation, high nibble the element format:
	// $8x = 8.8 fixed point (2 bytes), $9x = IEEE float32 (4 bytes).
	const u32 elemSize = ( ( op & 0xF0 ) == 0x80 ) ? 2 : 4;
	const u8 sub = op & 0x0F;

	u8 spaceA, spaceB, spaceC;
	u32 addrA, addrB, addrC;
	u8 res;

	switch ( sub )
	{
	case 0x00:					// MAT_MUL
	{
		u32 m = sArg[ 0 ], k = sArg[ 1 ], n = sArg[ 2 ];
		if ( m == 0 || k == 0 || n == 0 )
			return GPU64_ERR_BAD_ARGS;

		u32 bytesA = matBytes( m, k, elemSize );
		u32 bytesB = matBytes( k, n, elemSize );
		u32 bytesC = matBytes( m, n, elemSize );
		if ( bytesA == 0 || bytesB == 0 || bytesC == 0 )
			return GPU64_ERR_OUT_OF_RANGE;

		argCompact( 3, &spaceA, &addrA );
		argCompact( 7, &spaceB, &addrB );
		argCompact( 11, &spaceC, &addrC );

		res = gpu64_blobRead( spaceA, addrA, bytesA, sBlobA );
		if ( res != GPU64_ERR_OK ) return res;
		res = gpu64_blobRead( spaceB, addrB, bytesB, sBlobB );
		if ( res != GPU64_ERR_OK ) return res;

		for ( u32 i = 0; i < m; i++ )
			for ( u32 j = 0; j < n; j++ )
			{
				if ( elemSize == 2 )
				{
					s64 acc = 0;
					for ( u32 x = 0; x < k; x++ )
						acc += (s64)fixRead( sBlobA, i * k + x ) * (s64)fixRead( sBlobB, x * n + j );
					fixWrite( sBlobC, i * n + j, acc );
				} else
				{
					float acc = 0.0f;
					for ( u32 x = 0; x < k; x++ )
						acc += fltRead( sBlobA, i * k + x ) * fltRead( sBlobB, x * n + j );
					fltWrite( sBlobC, i * n + j, acc );
				}
			}

		return gpu64_blobWrite( spaceC, addrC, bytesC, sBlobC );
	}

	case 0x01:					// MAT_ADD
	case 0x02:					// MAT_SUB
	{
		u32 m = sArg[ 0 ], n = sArg[ 1 ];
		if ( m == 0 || n == 0 )
			return GPU64_ERR_BAD_ARGS;

		u32 bytes = matBytes( m, n, elemSize );
		if ( bytes == 0 )
			return GPU64_ERR_OUT_OF_RANGE;

		argCompact( 2, &spaceA, &addrA );
		argCompact( 6, &spaceB, &addrB );
		argCompact( 10, &spaceC, &addrC );

		res = gpu64_blobRead( spaceA, addrA, bytes, sBlobA );
		if ( res != GPU64_ERR_OK ) return res;
		res = gpu64_blobRead( spaceB, addrB, bytes, sBlobB );
		if ( res != GPU64_ERR_OK ) return res;

		for ( u32 i = 0; i < m * n; i++ )
		{
			if ( elemSize == 2 )
			{
				s32 a = fixRead( sBlobA, i ), b = fixRead( sBlobB, i );
				fixStore( sBlobC, i, ( sub == 0x01 ) ? a + b : a - b );
			} else
			{
				float a = fltRead( sBlobA, i ), b = fltRead( sBlobB, i );
				fltWrite( sBlobC, i, ( sub == 0x01 ) ? a + b : a - b );
			}
		}

		return gpu64_blobWrite( spaceC, addrC, bytes, sBlobC );
	}

	case 0x03:					// MAT_SCALE
	{
		u32 m = sArg[ 0 ], n = sArg[ 1 ];
		if ( m == 0 || n == 0 )
			return GPU64_ERR_BAD_ARGS;

		u32 bytes = matBytes( m, n, elemSize );
		if ( bytes == 0 )
			return GPU64_ERR_OUT_OF_RANGE;

		argCompact( 2, &spaceA, &addrA );
		argCompact( 6, &spaceC, &addrC );

		res = gpu64_blobRead( spaceA, addrA, bytes, sBlobA );
		if ( res != GPU64_ERR_OK ) return res;

		if ( elemSize == 2 )
		{
			s64 s = (s64)(s16)argU16( 10 );		// the scalar, inline in ARG10-11
			for ( u32 i = 0; i < m * n; i++ )
				fixWrite( sBlobC, i, (s64)fixRead( sBlobA, i ) * s );
		} else
		{
			float s = fltRead( &sArg[ 10 ], 0 );	// ARG10-13
			for ( u32 i = 0; i < m * n; i++ )
				fltWrite( sBlobC, i, fltRead( sBlobA, i ) * s );
		}

		return gpu64_blobWrite( spaceC, addrC, bytes, sBlobC );
	}

	case 0x04:					// MAT_TRANSPOSE
	{
		u32 m = sArg[ 0 ], n = sArg[ 1 ];
		if ( m == 0 || n == 0 )
			return GPU64_ERR_BAD_ARGS;

		u32 bytes = matBytes( m, n, elemSize );
		if ( bytes == 0 )
			return GPU64_ERR_OUT_OF_RANGE;

		argCompact( 2, &spaceA, &addrA );
		argCompact( 6, &spaceC, &addrC );

		res = gpu64_blobRead( spaceA, addrA, bytes, sBlobA );
		if ( res != GPU64_ERR_OK ) return res;

		for ( u32 i = 0; i < m; i++ )
			for ( u32 j = 0; j < n; j++ )
			{
				if ( elemSize == 2 )
					fixStore( sBlobC, j * m + i, fixRead( sBlobA, i * n + j ) );
				else
					fltWrite( sBlobC, j * m + i, fltRead( sBlobA, i * n + j ) );
			}

		return gpu64_blobWrite( spaceC, addrC, bytes, sBlobC );
	}

	case 0x05:					// MAT_IDENTITY
	{
		u32 n = sArg[ 0 ];
		if ( n == 0 )
			return GPU64_ERR_BAD_ARGS;

		u32 bytes = matBytes( n, n, elemSize );
		if ( bytes == 0 )
			return GPU64_ERR_OUT_OF_RANGE;

		argCompact( 1, &spaceC, &addrC );

		memset( sBlobC, 0, bytes );
		for ( u32 i = 0; i < n; i++ )
		{
			if ( elemSize == 2 )
				fixStore( sBlobC, i * n + i, 0x0100 );	// 1.0 in 8.8
			else
				fltWrite( sBlobC, i * n + i, 1.0f );
		}

		return gpu64_blobWrite( spaceC, addrC, bytes, sBlobC );
	}

	case 0x06:					// MAT_INVERSE
	{
		u32 n = sArg[ 0 ];
		if ( n == 0 )
			return GPU64_ERR_BAD_ARGS;

		u32 bytes = matBytes( n, n, elemSize );
		if ( bytes == 0 )
			return GPU64_ERR_OUT_OF_RANGE;

		argCompact( 1, &spaceA, &addrA );
		argCompact( 5, &spaceC, &addrC );

		res = gpu64_blobRead( spaceA, addrA, bytes, sBlobA );
		if ( res != GPU64_ERR_OK ) return res;

		res = matInverse( n, elemSize, sBlobA, sBlobC );
		if ( res != GPU64_ERR_OK )
			return res;				// singular: nothing written

		return gpu64_blobWrite( spaceC, addrC, bytes, sBlobC );
	}
	}

	return GPU64_ERR_BAD_OPCODE;
}

void gpu64_apiDispatch( u8 op )
{
	// gpu64: finish any page flip still in flight before anything else runs
	// (gpu64_flip.h). Two reasons, and both are correctness, not tidiness:
	// this command may draw into a page the VideoCore has not yet stopped
	// scanning out, and it may itself want Circle's mailbox, whose Flush()
	// would eat our reply at 20 ms a go. Normally a no-op -- the reply is
	// long since ready by the time the C64 issues its next command.
	gpu64_flipDrain();

	u8 res;

#ifdef GPU64_3D_ENABLED
	if ( sCmdHi == 1 )
	{
		// Class 1 does not execute here -- it stages a command and hands it
		// to core 1 (docs/milestone6_3d_design.md, Architecture). What comes
		// back is the *acceptance* result, not the command's outcome: a
		// class 1 OK means core 1 has the command, and STATUS bit4 or RESULT
		// is what says it finished. That is the whole point of the split.
		res = gpu64_3dDispatch( op );
	} else
#endif
#ifdef GPU64_RASTER_ENABLED
	if ( sCmdHi == 2 )
	{
		// Class 2 executes here and now, on core 0, exactly like a class 0
		// draw op -- the C64 is halted for the dispatch, so there is nothing
		// for another core to overlap with. See
		// docs/milestone8_raster_design.md.
		res = gpu64_rasterDispatch( op );
	} else
#endif
	if ( sCmdHi != 0 )
	{
		sErr = GPU64_ERR_BAD_CLASS;
		sStatus |= GPU64_STATUS_ERROR;
		return;
	} else
	if ( op < 0x10 )
		res = doSystem( op );
	else if ( op < 0x80 )
		res = doDraw( op );
	else if ( op < 0xA0 )
		res = doMath( op );
	else
		res = GPU64_ERR_BAD_OPCODE;

	sErr = res;
	if ( res == GPU64_ERR_OK )
	{
		sStatus &= ~GPU64_STATUS_ERROR;

		// gpu64: the log overlay is a bring-up aid for the default state.
		// Once a program owns the screen it should not have firmware text
		// painted over its output on every flip -- and PrepareFlip() was
		// walking 25 rows of glyphs per frame to do it. So the first
		// successful command of a session hides it, on the same "a program
		// engaged the API" transition that stops the mirror. LOG_ENABLE
		// itself is exempt, so a program that explicitly asks for the log
		// as its first command gets it.
		if ( !gpu64ApiActive && op != 0x07 )
		{
			CGpu64FrameBuffer *pFB = g_pGpu64FB;
			if ( pFB ) pFB->LogEnable( FALSE );
		}

		// A successful dispatch is what counts as "a program engaged the
		// API" -- deliberately not any write into the window, so an REU
		// detection routine scanning IO2 can't disarm the mirror by
		// accident the way the milestone 2 trigger could.
		// gpu64: the milestone 6 load ladder starts here rather than at
		// REU-session start (gpu64_ladder.h). The ladder's rungs are 4s
		// wall-clock windows, and the user spends an unknown -- and
		// unrepeatable -- amount of time in the RAD menu and at the BASIC
		// LOAD prompt before the test PRG runs. Anchoring the clock to the
		// first successful command puts the same rung under the same C64
		// workload on every run.
#ifdef GPU64_LADDER_ENABLED
		if ( !gpu64ApiActive )
			gpu64_ladderArm();
#endif

		gpu64ApiActive = 1;
	} else
		sStatus |= GPU64_STATUS_ERROR;
}
