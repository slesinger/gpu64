/*

  {_______            {_          {______
        {__          {_ __               {__
        {__         {_  {__               {__
     {__           {__   {__               {__
 {______          {__     {__              {__
       {__       {__       {__            {__   
         {_________         {______________		Expansion Unit
                
 RADExp - A framework for DMA interfacing with Commodore C64/C128 computers using a Raspberry Pi Zero 2 or 3A+/3B+
 Copyright (c) 2022-2026 Carsten Dachsbacher <frenetic@dachsbacher.de>

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

// required for C128
#define FORCE_RESET_VECTORS

#include "rad_main.h"
#include "build_id.h"
#include "dirscan.h"
#include "c64screen.h"
#include "linux/kernel.h"
#include "config.h"
#include "rad_iecdevice.h"

static const char DRIVE[] = "SD:";
static const char FILENAME_CONFIG[] = "SD:RAD/rad.cfg";

// REU
#include "rad_reu.h"
#define REU_MAX_SIZE_KB	(16384)
u8 mempool[ REU_MAX_SIZE_KB * 1024 + 8192 ] AAA = {0};
u8 *mempoolPtr = &mempool[ 0 ];
u8 prgLaunch[ 65536 + 2 ] AAA = {0};

// low-level communication code
u64 armCycleCounter;
#include "lowlevel_dma.h"

// GeoRAM
#include "rad_georam.h"

// VSF
u8 vsf[ 17 * 1024 * 1024 ] = {0};

void warmCache()
{
	for ( int i = min( REU_SIZE_KB, 256 ) * 1024 - 64; i >= 0; i -= 64 )
		CACHE_PRELOADL2KEEP( &mempool[ i ] );

	CACHE_PRELOAD_DATA_CACHE( &reu, sizeof( REUSTATE ), CACHE_PRELOADL1KEEP )
	FORCE_READ_LINEAR32a( &reu, sizeof( REUSTATE ), sizeof( REUSTATE ) * 8 );

	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)reuUsingPolling, 1024 * 7 );
	FORCE_READ_LINEARa( (void*)reuUsingPolling, 1024 * 7, 65536 );

	// gpu64: gpu64_mirrorSnapshot() is called from inside reuUsingPolling()'s
	// cycle-critical loop but is a separate function, so it isn't covered by
	// the preload above -- an instruction-cache miss mid-burst could blow the
	// per-byte DMA_READBYTE timing the same way an uncached reuUsingPolling()
	// itself would. Size is a guess (matches geoRAMUsingPolling's below);
	// unverified on real hardware.
	extern void gpu64_mirrorSnapshot( void );
	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)gpu64_mirrorSnapshot, 1024 * 2 );
	FORCE_READ_LINEARa( (void*)gpu64_mirrorSnapshot, 1024 * 2, 65536 );
}

void warmCacheGeoRAM()
{
	for ( int k = 0; k < 1024; k += 64 )
		CACHE_PRELOADL2STRM( &mempool[ k ] );

	FORCE_READ_LINEARa( &mempool[ 0 ], 1024, 1024 * 64 );

	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)geoRAMUsingPolling, 1024 * 2 );
	FORCE_READ_LINEARa( (void*)geoRAMUsingPolling, 1024 * 2, 65536 );
}

static u16 resetVector = 0xFCE2;

// emulate GAME-cartridge to start C128 (also works on C64) with custom reset-vector => forces C128 in C64 mode
void startForcedResetVectors()
{
	register u32 g2, g3;

	const u8 romh[] = { 
		0x4c, 0x0a, 0xe5, 0x4c, 0x00, 0xe5, 0x52, 0x52,
		0x42, 0x59, 0x43, 0xfe, 0xe2, 0xfc, 0x48, 0xff };

	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)startForcedResetVectors, 1024 * 4 );
	CACHE_PRELOADL1STRM( romh );
	FORCE_READ_LINEARa( (void*)startForcedResetVectors, 1024 * 4, 65536 );
	FORCE_READ_LINEARa( (void*)romh, 16, 1024 );

	OUT_GPIO( DMA_OUT );
	OUT_GPIO( GAME_OUT );

	WAIT_FOR_CPU_HALFCYCLE
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER
	WAIT_UP_TO_CYCLE( 100 );
	SET_GPIO( bLATCH_A_OE | bIRQ_OUT | bOE_Dx | bRW_OUT | bDMA_OUT | bDIR_Dx );
	INP_GPIO_RW();
	INP_GPIO_IRQ();

	CLR_GPIO( bGAME_OUT );
	CLR_GPIO( bMPLEX_SEL );

	DELAY( 1 << 20 );
	SET_GPIO( bRESET_OUT );
	INP_GPIO( RESET_OUT );

	u32 nCycles = 0, nRead = 0;
	while ( 1 )
	{
		WAIT_FOR_CPU_HALFCYCLE
		RESTART_CYCLE_COUNTER
		WAIT_UP_TO_CYCLE( 50 );
		RESTART_CYCLE_COUNTER
		WAIT_UP_TO_CYCLE( WAIT_FOR_SIGNALS );
		g2 = read32( ARM_GPIO_GPLEV0 );

		SET_GPIO( bMPLEX_SEL );
		WAIT_UP_TO_CYCLE( WAIT_CYCLE_MULTIPLEXER );
		g3 = read32( ARM_GPIO_GPLEV0 );
		CLR_GPIO( bMPLEX_SEL );

		if ( nCycles ++ > 100000 )
		{
			OUT_GPIO( RESET_OUT );
			CLR_GPIO( bRESET_OUT );
			DELAY( 1 << 18 );
			SET_GPIO( bRESET_OUT );
			INP_GPIO( RESET_OUT );
			nRead = nCycles = 0;
		}

		if ( ROMH_ACCESS && CPU_READS_FROM_BUS )
		{
			u8 d = 0;
			if ( ADDRESS0to7 == 0xfc || ADDRESS0to7 == 0xfd ) nRead ++;
			if ( ADDRESS0to7 == 0xfc ) d = resetVector & 255;
			if ( ADDRESS0to7 == 0xfd ) d = resetVector >> 8;

			{
				register u32 DD = ( ( d ) & 255 ) << D0;
				write32( ARM_GPIO_GPCLR0, ( D_FLAG & ( ~DD ) ) | bOE_Dx | bDIR_Dx );
				write32( ARM_GPIO_GPSET0, DD );
				SET_BANK2_OUTPUT
				WAIT_UP_TO_CYCLE( WAIT_CYCLE_READ );
				SET_GPIO( bOE_Dx | bDIR_Dx );
			}

			if ( nRead >= 2 )
			{
				WAIT_FOR_VIC_HALFCYCLE
				SET_GPIO( bGAME_OUT );
				break;
			}
		}
		WAIT_FOR_VIC_HALFCYCLE
		RESET_CPU_CYCLE_COUNTER
	}
}


//#define REU_PROTOCOL
#ifdef REU_PROTOCOL
REUPROT reuProtocol[ 65536 ];
u32 nReuProtocol = 0;
#endif

#include "rad_hijack.h"

volatile u8 bla = 0;

u8 reuImageIsNuvie( u8 *m )
{
	u8 pat1[ 16 ] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };	// 0x8f00f0
	u8 pat2[ 7 ] = { 0x30, 0x30, 0x31, 0x16, 0x31, 0x2e, 0x30 }; // 0x0000f5
	if ( memcmp( pat1, &m[ 0x8f00f0 ], 16 ) == 0 || memcmp( pat2, &m[ 0x0000f5 ], 7 ) == 0 )
		return SPECIAL_NUVIE;
	return 0;
}

bool reuImageIsBlureu( u8 *m, u32 size )
{
   	u32 crc = 0xffffffff;
	for ( u32 i = 0; i < size; i++ )
	{
      	crc ^= *(m++);
      	for ( u8 j = 0; j < 8; j++ ) 
        	crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
	}
	if ( crc == 0xd569cb25 )
		return SPECIAL_BLUREU;
	return 0;
}

u32 temperature;

#define WAIT_FOR_READY_PROMPT \
	int done;												\
	do {													\
		for ( u32 i = 0; i < radWaitCycles; i++ ) 					\
			emuWAIT_FOR_VIC_HALFCYCLE						\
		done = checkForReadyPrompt( !go64mode );			\
	} while ( !done );


// gpu64: draw a checkerboard test pattern directly into m_Screen's already-
// initialized framebuffer (proven working by the fact CRAD::Initialize()
// returned TRUE), scaled to whatever resolution it actually negotiated.
void CRAD::showTestPattern( void )
{
	// gpu64 debug checkpoint: no UART, no confirmed HDMI yet, so use the
	// ACT LED itself as a crude status channel -- distinct from the later
	// hijack-loop breathing pattern, this fires once, early, right after
	// power-on. 2 blinks = entered the function; then a pause; then a
	// blink count equal to min(w,h)/100 reports the framebuffer dimensions
	// we actually got (e.g. 19 blinks for a 1920-wide buffer) so we can
	// tell whether m_Screen's framebuffer looks sane at all without any
	// other output channel.
	CActLED bootLED;
	bootLED.Blink( 2, 200, 200 );

	unsigned wFull = m_Screen.GetWidth();
	unsigned hFull = m_Screen.GetHeight();

	// gpu64: confined to the reserved top-left box (see tee_device.h) so the
	// pattern never collides with the HDMI on-screen log below it.
	unsigned w = min( wFull, (unsigned)GPU_OUTPUT_BOX_W );
	unsigned h = min( hFull, (unsigned)GPU_OUTPUT_BOX_H );

	DELAY( 1 << 24 );
	bootLED.Blink( min( wFull, hFull ) / 100, 120, 200 );

	// gpu64: this is called both once at boot and again on every C64-side
	// trigger ($DF0B write, see rad_reu.cpp) -- with an identical pattern
	// each time, a real hardware tester has no way to tell a fresh trigger
	// from the leftover boot-time draw just by looking at the screen (this
	// came up during hw testing: the log line "showTestPattern: drawing..."
	// was the only real proof a trigger fired). callCount inverts which
	// squares are lit vs. black on alternating calls, and gets logged, so
	// each invocation is visibly distinct on screen as well as in the log.
	static unsigned callCount = 0;
	callCount++;
	boolean bInvert = ( callCount & 1 ) == 0;

	logger->Write( "gpu64", LogNotice, "showTestPattern: drawing %ux%u (screen %ux%u), call #%u%s", w, h, wFull, hFull, callCount, bInvert ? " (inverted)" : "" );

	for ( unsigned y = 0; y < h; y++ )
	{
		for ( unsigned x = 0; x < w; x++ )
		{
			boolean bLit = ( ( x / 20 ) + ( y / 20 ) ) & 1;
			if ( bInvert )
				bLit = !bLit;
			if ( !bLit )
			{
				m_Screen.SetPixel( x, y, BLACK_COLOR );
				continue;
			}
			// rainbow gradient across the lit squares, cycling through the
			// full 15-bit RGB565-ish COLOR16 range
			u8 r = (u8)( ( x * 31 ) / w );
			u8 g = (u8)( ( y * 31 ) / h );
			u8 b = (u8)( ( ( x + y ) * 31 / ( w + h ) ) );
			m_Screen.SetPixel( x, y, COLOR16( r, g, b ) );
		}
	}

	// The CPU writes above only land in cache until explicitly cleaned to
	// DRAM -- the GPU/HVS scans out framebuffer memory directly and isn't
	// cache-coherent with the ARM core, so without this the display just
	// keeps showing whatever was there before (black), no matter what
	// SetPixel() does.
	CBcmFrameBuffer *pFB = m_Screen.GetFrameBuffer();
	CleanDataCacheRange( (u64)(uintptr)pFB->GetBuffer(), pFB->GetSize() );

	DELAY( 1 << 24 );
	bootLED.Blink( 9, 200, 200 );	// 9 blinks = drawing + cache-clean finished, function returning

	logger->Write( "gpu64", LogNotice, "showTestPattern: done" );
}

// gpu64: checkpoint marker strip -- see CRAD::mark()'s declaration in
// rad_main.h. Squares sit in the band between GPU_OUTPUT_BOX's bottom edge and
// the screen bottom, left of the log column (HDMI_LOG_X0), so nothing else
// draws over them. Each index gets its own fixed colour and x position, and a
// lit square is permanent for the rest of the session -- the tester just reads
// off how far execution got, even if no log line ever appears.
#define GPU64_MARK_Y0		( GPU_OUTPUT_BOX_H + 10 )
#define GPU64_MARK_SIZE		40
#define GPU64_MARK_PITCH	60

void CRAD::mark( unsigned idx )
{
	static const TScreenColor markColor[ 8 ] =
	{
		COLOR16( 31, 31, 31 ),	// 0 white
		COLOR16( 31,  0,  0 ),	// 1 red
		COLOR16(  0, 31,  0 ),	// 2 green
		COLOR16(  0,  0, 31 ),	// 3 blue
		COLOR16( 31, 31,  0 ),	// 4 yellow
		COLOR16( 31,  0, 31 ),	// 5 magenta
		COLOR16(  0, 31, 31 ),	// 6 cyan
		COLOR16( 31, 16,  0 )	// 7 orange
	};

	unsigned wFull = m_Screen.GetWidth();
	unsigned hFull = m_Screen.GetHeight();

	unsigned x0 = 10 + ( idx & 7 ) * GPU64_MARK_PITCH;
	unsigned y0 = GPU64_MARK_Y0;

	// Clamp rather than bail out. The first hardware run of these markers
	// reported "no squares at all" on a path that other evidence says did
	// execute, and an earlier revision returned silently whenever the strip
	// did not fit -- which would produce exactly that symptom on any display
	// mode shorter than GPU64_MARK_Y0 + GPU64_MARK_SIZE. A clamped square is
	// still unambiguous; a silently skipped one is indistinguishable from
	// code that never ran, which is the whole thing these markers exist to
	// tell apart.
	if ( wFull < GPU64_MARK_SIZE || hFull < GPU64_MARK_SIZE )
		return;
	if ( x0 + GPU64_MARK_SIZE > wFull )
		x0 = wFull - GPU64_MARK_SIZE;
	if ( y0 + GPU64_MARK_SIZE > hFull )
		y0 = hFull - GPU64_MARK_SIZE;

	TScreenColor col = markColor[ idx & 7 ];

	for ( unsigned y = y0; y < y0 + GPU64_MARK_SIZE; y++ )
		for ( unsigned x = x0; x < x0 + GPU64_MARK_SIZE; x++ )
			m_Screen.SetPixel( x, y, col );

	// only clean the rows actually touched -- one of the call sites is inside
	// reuUsingPolling()'s cycle-critical loop, where a full-framebuffer clean
	// would blow the timing budget (same reasoning as CHDMIConsole::Write()).
	CBcmFrameBuffer *pFB = m_Screen.GetFrameBuffer();
	u32 nPitch = pFB->GetPitch();
	CleanDataCacheRange( (u64)(uintptr)( pFB->GetBuffer() + y0 * nPitch ), GPU64_MARK_SIZE * nPitch );
}

void gpu64_mark( unsigned idx )
{
	if ( g_pRAD )
		g_pRAD->mark( idx );
}

// gpu64: stage indicator for gpu64_mirrorSnapshot(). Unlike mark(), which
// latches (each square stays lit forever), this repaints ONE square whose
// colour says which stage the snapshot last completed -- so when the loop
// freezes, the colour left on screen names the statement it froze in.
//
// Needed because the freeze is a hard hang: the RAD menu button stopped
// responding, and that check lives in reuUsingPolling()'s loop, so the loop
// itself is stuck rather than merely failing to draw. The prime suspects are
// the unbounded BA-wait spins inside DMA_READBYTE_P1/P2 (they loop until the
// VIC releases the bus, forever if it never does), and those sit between
// stages 1 and 4.
//
// Deliberately small (16x16) and logger-free: stages 2 and 3 are painted
// while DMA is held. The C64's CPU is stopped then, so the delay is harmless
// to it, but there is no reason to make it bigger than it needs to be.
#define GPU64_STAGE_X		600
#define GPU64_STAGE_SIZE	16

void CRAD::stage( unsigned s )
{
	static const TScreenColor stageColor[ 8 ] =
	{
		COLOR16(  8,  8,  8 ),	// 0 grey   - idle/never entered
		COLOR16( 31,  0,  0 ),	// 1 red    - about to grab DMA
		COLOR16( 31, 16,  0 ),	// 2 orange - screen RAM read done
		COLOR16( 31, 31,  0 ),	// 3 yellow - colour RAM read done
		COLOR16(  0, 31,  0 ),	// 4 green  - DMA released
		COLOR16(  0, 31, 31 ),	// 5 cyan   - showMirror() returned
		COLOR16(  0,  0, 31 ),	// 6 blue   - unused
		COLOR16( 31,  0, 31 )	// 7 magenta- unused
	};

	unsigned wFull = m_Screen.GetWidth();
	unsigned hFull = m_Screen.GetHeight();

	unsigned x0 = GPU64_STAGE_X, y0 = GPU64_MARK_Y0;
	if ( x0 + GPU64_STAGE_SIZE > wFull || y0 + GPU64_STAGE_SIZE > hFull )
		return;

	TScreenColor col = stageColor[ s & 7 ];
	for ( unsigned y = y0; y < y0 + GPU64_STAGE_SIZE; y++ )
		for ( unsigned x = x0; x < x0 + GPU64_STAGE_SIZE; x++ )
			m_Screen.SetPixel( x, y, col );

	CBcmFrameBuffer *pFB = m_Screen.GetFrameBuffer();
	u32 nPitch = pFB->GetPitch();
	CleanDataCacheRange( (u64)(uintptr)( pFB->GetBuffer() + y0 * nPitch ), GPU64_STAGE_SIZE * nPitch );
}

void gpu64_stage( unsigned s )
{
	if ( g_pRAD )
		g_pRAD->stage( s );
}

// gpu64: only one CRAD is ever constructed (see main() below); g_pRAD is set
// from its constructor (rad_main.h). This wrapper lets rad_reu.cpp's bus-hijack
// loop trigger the pattern from the C64 side (IO2 $DF0B write) without pulling
// in rad_main.h's full Circle/screen include stack -- see the forward
// declaration in rad_reu.cpp.
CRAD *g_pRAD = nullptr;

// gpu64: diagnostic-only counter + log helper, called from
// gpu64_mirrorSnapshot() (rad_reu.cpp) right before it grabs the DMA burst
// -- see the comment there. Counts calls so the log line makes it obvious
// this is actually firing repeatedly, not just once.
// Throttled: at 4 snapshots/sec, logging every one filled the whole log column
// in about fifteen seconds and buried everything else, and each line is real
// work inside the polling loop (glyph rendering via SetPixel plus a cache
// clean). One line every GPU64_MIRROR_LOG_EVERY snapshots is enough to show
// the poll is alive; the heartbeat square and the ACT LED cover the
// per-snapshot case without touching the log at all.
#define GPU64_MIRROR_LOG_EVERY	40

static unsigned gpu64_mirrorSnapshotStartCount = 0;
void gpu64_logMirrorSnapshotStart( void )
{
	gpu64_mirrorSnapshotStartCount++;
	if ( ( gpu64_mirrorSnapshotStartCount % GPU64_MIRROR_LOG_EVERY ) == 1 )
		logger->Write( "gpu64", LogNotice, "gpu64_mirrorSnapshot: starting burst #%u", gpu64_mirrorSnapshotStartCount );
}

unsigned gpu64_mirrorSnapshotCount( void )
{
	return gpu64_mirrorSnapshotStartCount;
}

void gpu64_showTestPattern( CRAD *pRAD )
{
	if ( pRAD )
		pRAD->showTestPattern();
}

// gpu64: standard C64 16-color palette (Pepto's commonly-used values, 8-bit
// per channel here; showMirror() below shifts down to the 5-bit-per-channel
// range COLOR16() expects). Not read from hardware -- color RAM only ever
// stores a 4-bit index (0-15) into this fixed, well-known palette, there's
// nothing to sniff.
static const u8 c64Palette[ 16 ][ 3 ] = {
	{   0,   0,   0 },		// 0 black
	{ 255, 255, 255 },		// 1 white
	{ 104,  55,  43 },		// 2 red
	{ 112, 164, 178 },		// 3 cyan
	{ 111,  61, 134 },		// 4 purple
	{  88, 141,  67 },		// 5 green
	{  53,  40, 121 },		// 6 blue
	{ 184, 199, 111 },		// 7 yellow
	{ 111,  79,  37 },		// 8 orange
	{  67,  57,   0 },		// 9 brown
	{ 154, 103,  89 },		// 10 light red
	{  68,  68,  68 },		// 11 dark grey
	{ 108, 108, 108 },		// 12 grey
	{ 154, 210, 132 },		// 13 light green
	{ 108,  94, 181 },		// 14 light blue
	{ 149, 149, 149 },		// 15 light grey
};

// gpu64: extern rather than #include "font.h" -- that header is a raw
// generated .bin-to-array dump with no include guard and no `extern`,
// designed for exactly one translation unit (rad_hijack.cpp already includes
// it) to own the storage; including it a second time here would duplicate
// the 4KB array and fail to link. font_bin only gets mutated as menu-logo
// scratch space during the earlier hijack/menu phase (see tee_device.h's
// comment on why CHDMIConsole avoids it for that reason) -- by the time
// showMirror() runs (during REU emulation, a later phase), that mutation is
// long done and font_bin is stable to read from.
extern unsigned char font_bin[ 4096 ];

void CRAD::showMirror( const u8 *screen, const u8 *color, u8 border, u8 background )
{
	unsigned wFull = m_Screen.GetWidth();
	unsigned hFull = m_Screen.GetHeight();

	// 40x25 text cells @ 8x8 pixels = 320x200, doubled to 640x400 to fill
	// most of the reserved GPU_OUTPUT_BOX (700x460, see tee_device.h) --
	// same box showTestPattern() draws into, since the two are mutually
	// exclusive at any given moment.
	const unsigned scale = 2;
	unsigned w = min( wFull, (unsigned)( 320 * scale ) );
	unsigned h = min( hFull, (unsigned)( 200 * scale ) );

	TScreenColor bgCol = COLOR16( c64Palette[ background ][ 0 ] >> 3, c64Palette[ background ][ 1 ] >> 3, c64Palette[ background ][ 2 ] >> 3 );

	for ( unsigned cy = 0; cy < 25; cy++ )
	{
		for ( unsigned cx = 0; cx < 40; cx++ )
		{
			unsigned cellIdx = cy * 40 + cx;
			u8 code = screen[ cellIdx ];
			const u8 *glyph = &font_bin[ (unsigned)code * 8 ];
			TScreenColor fgCol = COLOR16( c64Palette[ color[ cellIdx ] ][ 0 ] >> 3, c64Palette[ color[ cellIdx ] ][ 1 ] >> 3, c64Palette[ color[ cellIdx ] ][ 2 ] >> 3 );

			for ( unsigned gy = 0; gy < 8; gy++ )
			{
				u8 rowBits = glyph[ gy ];
				unsigned py = ( cy * 8 + gy ) * scale;
				if ( py >= h )
					continue;

				for ( unsigned gx = 0; gx < 8; gx++ )
				{
					boolean bLit = ( rowBits & ( 0x80 >> gx ) ) != 0;
					TScreenColor col = bLit ? fgCol : bgCol;
					unsigned px = ( cx * 8 + gx ) * scale;
					if ( px >= w )
						continue;

					for ( unsigned sy = 0; sy < scale; sy++ )
						for ( unsigned sx = 0; sx < scale; sx++ )
							m_Screen.SetPixel( px + sx, py + sy, col );
				}
			}
		}
	}

	// gpu64: two heartbeats, deliberately on channels that fail independently.
	// The mirror was observed freezing after 14 snapshots -- both the mirror
	// image and the on-screen log stopped at once, which could equally mean
	// "the polling loop died" or "HDMI writes stopped landing in DRAM". These
	// tell those apart in a single run: the square is an HDMI write that does
	// not go through CHDMIConsole, the ACT LED is not HDMI at all.
	static u8 heartbeat = 0;
	heartbeat ^= 1;

	unsigned hbX = 500, hbY = GPU64_MARK_Y0;
	if ( hbX + GPU64_MARK_SIZE <= wFull && hbY + GPU64_MARK_SIZE <= hFull )
	{
		TScreenColor hbCol = heartbeat ? COLOR16( 31, 31, 31 ) : COLOR16( 0, 0, 15 );
		for ( unsigned y = hbY; y < hbY + GPU64_MARK_SIZE; y++ )
			for ( unsigned x = hbX; x < hbX + GPU64_MARK_SIZE; x++ )
				m_Screen.SetPixel( x, y, hbCol );
	}

	// On()/Off() are plain GPIO writes -- unlike CActLED::Blink(), which
	// sleeps, and must never be called from here (this runs with the C64
	// free-running and the polling loop not servicing the bus). Own instance
	// rather than CActLED::Get(): the only other CActLED in this file is
	// showTestPattern()'s local, which is long destructed by now.
	static CActLED mirrorLED;
	if ( heartbeat )
		mirrorLED.On();
	else
		mirrorLED.Off();

	// same cache-clean requirement as showTestPattern() -- SetPixel() writes
	// only land in cache until explicitly cleaned to DRAM.
	//
	// gpu64: only the rows actually drawn, not the whole framebuffer. At
	// 1824x984x2 a full clean is ~3.6MB per snapshot, four times a second,
	// all of it spent outside reuUsingPolling()'s loop with the C64
	// free-running -- a needlessly long window in which no bus access is
	// being serviced. The mirror occupies rows 0..h and the heartbeat square
	// its own strip, so clean exactly those two.
	CBcmFrameBuffer *pFB = m_Screen.GetFrameBuffer();
	u32 nPitch = pFB->GetPitch();
	u64 nBuffer = (u64)(uintptr)pFB->GetBuffer();
	CleanDataCacheRange( nBuffer, h * nPitch );
	if ( hbY + GPU64_MARK_SIZE <= hFull )
		CleanDataCacheRange( nBuffer + hbY * nPitch, GPU64_MARK_SIZE * nPitch );

	// throttled for the same reason as the "starting burst" line -- see
	// GPU64_MIRROR_LOG_EVERY above. Kept in step with that counter so the two
	// lines still appear as a pair for whichever snapshot does get logged.
	extern unsigned gpu64_mirrorSnapshotCount( void );
	if ( ( gpu64_mirrorSnapshotCount() % GPU64_MIRROR_LOG_EVERY ) == 1 )
		logger->Write( "gpu64", LogNotice, "showMirror: drew 40x25 snapshot (border=%u bg=%u)", border, background );
}

// gpu64: same free-function indirection as gpu64_showTestPattern() above --
// lets rad_reu.cpp's gpu64_mirrorSnapshot() reach CRAD::showMirror() without
// pulling in the full Circle/screen include stack.
void gpu64_showMirror( CRAD *pRAD, const u8 *screen, const u8 *color, u8 border, u8 background )
{
	if ( pRAD )
		pRAD->showMirror( screen, color, border, background );
}

void CRAD::Run( void )
{
	// gpu64: first thing logged, every boot. The card's config.txt names the
	// image the Pi loads (kernel_rad.img), which for a long time was NOT the
	// name build.sh deployed to -- so the hardware silently ran stale
	// firmware while three rounds of newly added diagnostics "produced no
	// output". Stamping the build into the log makes that failure mode
	// self-evident: if this line doesn't match the build you just deployed,
	// nothing below it is telling you anything about your current source.
	// GPU64_BUILD_ID is regenerated by tools/build.sh on every run
	// (Source/Firmware/build_id.h, git-ignored).
	logger->Write( "gpu64", LogNotice, "Run: bc0 build " GPU64_BUILD_ID );

	showTestPattern();

	m_EMMC.Initialize();
	logger->Write( "gpu64", LogNotice, "Run: bc1 EMMC.Initialize done" );

	EnableIRQs();
	initSerialOverUSB_IECDevice( &m_Interrupt, &m_Timer, &m_DeviceNameService, false );
	logger->Write( "gpu64", LogNotice, "Run: bc2 initSerialOverUSB_IECDevice done" );

	gpioInit();
	logger->Write( "gpu64", LogNotice, "Run: bc3 gpioInit (2nd call) done" );

	setDefaultTimings( AUTO_TIMING_RPI3PLUS_C64C128 );
	readConfig( logger, DRIVE, FILENAME_CONFIG );
	logger->Write( "gpu64", LogNotice, "Run: bc4 readConfig done" );

	OUT_GPIO( RESET_OUT );
	CLR_GPIO( bRESET_OUT );
	DELAY( 1 << 25 );
	SET_GPIO( bRESET_OUT );
	INP_GPIO( RESET_OUT );
	logger->Write( "gpu64", LogNotice, "Run: bc5 GPIO reset pulse done" );


	DisableIRQs();
	logger->Write( "gpu64", LogNotice, "Run: bc6 DisableIRQs done" );

	register u32 g2;

	// this also initializes timing values
	REU_SIZE_KB = 128;
	initREU( mempool );
	logger->Write( "gpu64", LogNotice, "Run: bc7 initREU done" );

	initHijack();
	logger->Write( "gpu64", LogNotice, "Run: bc8 initHijack done" );

	// gpu64: reference marker 7 (orange), drawn here where logging is known
	// to still work -- gives the tester a "this is what a lit square looks
	// like, and this is where to look for the others" reference, so a later
	// blank strip can be read as "that code never ran" rather than "maybe I
	// was looking at the wrong part of the screen". The log line alongside it
	// records the actual framebuffer geometry the strip was placed against.
	mark( 7 );
	logger->Write( "gpu64", LogNotice, "Run: bc8b mark strip y0=%u size=%u screen=%ux%u",
		(unsigned)GPU64_MARK_Y0, (unsigned)GPU64_MARK_SIZE, m_Screen.GetWidth(), m_Screen.GetHeight() );

#ifdef REU_PROTOCOL
	nReuProtocol = 0;
#endif

	#ifdef FORCE_RESET_VECTORS
	resetVector = 0xfce2;
	#endif

	u32 prgSize = 0;
	u8 isC128PRG = 0;
	u8 go64mode = 0;
	int res = 0;

	DisableIRQs();

	checkIfMachineRunning();		
	DELAY( 1 << 27 );

	while ( 1 )
	{
		prgSize = 0;
		isC128PRG = 0;
		go64mode = 0;
		res = 0;

		extern u32 radStartup, radStartupSize, radSilentMode, radWaitCycles;
		if ( radStartup == 1 )
		{
			meSize0 = radStartupSize;
			radLoadREUImage = radLaunchPRG = false;
			#ifdef STATUS_MESSAGES
			setStatusMessage( &statusMsg[ 0 ], " " );
			setStatusMessage( &statusMsg[ 80 ], " " );
			#endif
			goto startREUEmulation;
		} else
		if ( radStartup == 2 )
		{
			meSize1 = radStartupSize;
			radLoadGeoImage = radLaunchPRG = radLaunchGEORAM = false;
			#ifdef STATUS_MESSAGES
			setStatusMessage( &statusMsg[ 0 ], " " );
			setStatusMessage( &statusMsg[ 80 ], " " );
			#endif
			goto startGeoRAMEmulation;
		} else
		if ( radStartup == 3 )
		{
			goto hijacking;
		}


	radIsWaiting:
		CLR_GPIO( bMPLEX_SEL );

		while ( 1 )
		{
			RESTART_CYCLE_COUNTER						
			WAIT_UP_TO_CYCLE( 1250 );	
			g2 = read32( ARM_GPIO_GPLEV0 );			
	
			if ( BUTTON_PRESSED ) 
				goto hijacking;
		}


	hijacking:
		// gpu64: m_CPUThrottle removed (see rad_main.h) -- its mailbox calls hang
		// on this board. temperature is only used for the on-screen menu display.
		temperature = 0;

		SyncDataAndInstructionCache();
		CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)hijackC64, 1024 * 10 );
		FORCE_READ_LINEARa( (void*)hijackC64, 1024 * 10, 65536 );

		extern u8 justBooted;
		justBooted = 1;

		///////////////////////////////////////////////////////////////////////
		//
		// goto menu
		//
		///////////////////////////////////////////////////////////////////////

		res = hijackC64( false );			// after hijackC64 the CPU is still halted by DMA

		// gpu64: marker 0 (white) -- deliberately BEFORE the log call below,
		// so a lit square with no matching log line means logging (not the
		// code path) is what's broken here. See CRAD::mark() for the strip
		// layout and the full ladder of markers on this path (0..6).
		mark( 0 );

		// gpu64: diagnostic -- the stretch between here and reuUsingPolling()
		// starting up had no logging at all, which made a real hardware hang
		// (found to be gpu64ApiActive getting stuck at 1, see resetREU() in
		// rad_reu.cpp) indistinguishable from several other silent-forever
		// possibilities (WAIT_FOR_READY_PROMPT never seeing "READY.",
		// startForcedResetVectors()'s unbounded loop, dirscan.cpp's
		// already-marked-file re-launch trap). This one line at least proves
		// hijackC64() returned and shows what it decided.
		logger->Write( "gpu64", LogNotice, "Run: hijackC64 returned res=%d radLaunchPRG=%d radLaunchVSF=%d", res, (int)radLaunchPRG, (int)radLaunchVSF );

		WAIT_FOR_CPU_HALFCYCLE
		WAIT_FOR_VIC_HALFCYCLE
		RESTART_CYCLE_COUNTER
		SET_GPIO( bLATCH_A_OE | bIRQ_OUT | bGAME_OUT | bOE_Dx | bRW_OUT | bDMA_OUT );
		INP_GPIO( RW_OUT );
		INP_GPIO( IRQ_OUT );
		OUT_GPIO( RESET_OUT );
		CLR_GPIO( bRESET_OUT );

		// do we want to boot a C128 in C64-mode?
		go64mode = 0;

		// load PRG (if any) to figure out whether we want to force a C128 into C64-mode
		prgSize = 0;
		if ( radLaunchPRG )
		{
			readFile( logger, (char*)DRIVE, (char*)radLaunchPRGFile, prgLaunch, &prgSize );
			isC128PRG = *(u16*)prgLaunch == 0x1c01 ? 1 : 0;

			if ( isC128 )
			{
				if ( !isC128PRG && radLaunchPRG_NORUN_128 && !( *(u16 *)prgLaunch == 0x0801 ) )
					go64mode = 0; else
				{
					if ( ( !isC128PRG && radLaunchPRG ) || radLaunchGEORAM )
						go64mode = 1;
				}
			}

			// if we're not launching automatically: be nice and print load address
			if ( !isC128PRG && !( *(u16 *)prgLaunch == 0x0801 ) )
			{
				char *strAddr = strstr( &statusMsg[ 120 ], "$0000)" );
				char tmp[ 40 ];
				sprintf( tmp, "$%04X", *(u16 *)prgLaunch );
				for ( int i = 0; i < 5; i++ )
					strAddr[ i ] = tmp[ i ];
				memcpy( &statusMsg[ 80 ], &statusMsg[ 120 ], 40 );
			}
		}

		if ( radLaunchGEORAM || radLaunchVSF )
			go64mode = 1;

		//
		// let the machine start booting (meanwhile we'll load or initialize the REU/GeoRAM image and emulation)
		//
		CLR_GPIO( bMPLEX_SEL );
	#ifdef FORCE_RESET_VECTORS
		if ( go64mode )
		{
			startForcedResetVectors();
		} else
	#endif
		{
			DELAY( 1 << 18 );
			SET_GPIO( bRESET_OUT );
			INP_GPIO( RESET_OUT );
		}

		if ( res == RUN_REBOOT )
		{
			reboot(); 
		} else
		///////////////////////////////////////////////////////////////////////
		//
		// REU emulation
		//
		///////////////////////////////////////////////////////////////////////
		if ( res == RUN_MEMEXP + 1 )
		{
		startREUEmulation:
			mark( 1 );	// gpu64: red -- REU emulation branch taken
			REU_SIZE_KB = 128 << meSize0;
			initREU(mempool);
			resetREU();

			if ( radLoadREUImage )
			{
				u32 size;
				readFile( logger, (char*)DRIVE, (char*)radImageSelectedFile, mempool, &size );

				reu.isSpecial = reuImageIsNuvie( mempool );
				if ( !reu.isSpecial )
					reu.isSpecial = reuImageIsBlureu( mempool, size );
		} else
			{
				memset( mempool, 0, reu.reuSize );
				#ifdef STATUS_MESSAGES
				char tmp[ 40 ];
				sprintf( tmp, "%dK REU", reu.reuSize / 1024 );
				setStatusMessage( &statusMsg[ 0 ], tmp );
				#endif
			}

			reu.isModified = 0;

			mark( 2 );	// gpu64: green -- REU image loaded/initialized, about to wait for "READY."

			if ( radLaunchPRG )
			{
				// wait for "READY." to appear on screen
				WAIT_FOR_READY_PROMPT
				injectAndStartPRG( prgLaunch, prgSize, true );
			} else
			if ( radSilentMode != 0xffffffff )
			{
				// wait for "READY." to appear on screen
				WAIT_FOR_READY_PROMPT
				injectMessage( false );
			}

			mark( 3 );	// gpu64: blue -- PRG injected/started, about to warm caches

			SyncDataAndInstructionCache();
			warmCache();

			for ( u32 i = 0; i < 1000; i++ )
				emuWAIT_FOR_VIC_HALFCYCLE

			CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)reuUsingPolling, 1024 * 7 );
			FORCE_READ_LINEARa( (void*)reuUsingPolling, 1024 * 7, 65536 );

			resetREU();

			// gpu64: diagnostic, see the "hijackC64 returned" log above --
			// this is the last thing logged before reuUsingPolling() takes
			// over for good (it only logs again once the mirror poll or the
			// $DF0B trigger fires). gpu64ApiActive is logged specifically
			// because it used to get stuck at 1 from a previous session (see
			// resetREU() in rad_reu.cpp, now cleared there too) -- if this
			// ever prints 1 again right after a fresh resetREU(), that fix
			// regressed.
			extern u8 gpu64ApiActive;
			mark( 4 );	// gpu64: yellow -- last checkpoint before reuUsingPolling() takes over
			logger->Write( "gpu64", LogNotice, "Run: entering reuUsingPolling, gpu64ApiActive=%d", (int)gpu64ApiActive );

			reuUsingPolling();
		} else
		///////////////////////////////////////////////////////////////////////
		//
		// GeoRAM emulation
		//
		///////////////////////////////////////////////////////////////////////
		if ( res == RUN_MEMEXP + 2 )
		{
		startGeoRAMEmulation:
			// GeoRAM
			geoSizeKB = 512 << meSize1;

			geoRAM_Init();

			if ( radLoadGeoImage )
			{
				u32 size;
				static const char DRIVE[] = "SD:";
				readFile( logger, (char*)DRIVE, (char*)radImageSelectedFile, geo.RAM, &size );
			} else
			{
				#ifdef STATUS_MESSAGES
				char tmp[ 40 ];
				sprintf( tmp, "%dK GEORAM", geoSizeKB );
				setStatusMessage( &statusMsg[ 0 ], tmp );
				#endif
			}

			if ( radLaunchPRG )
			{
				// wait for "READY." to appear on screen
				WAIT_FOR_READY_PROMPT
				injectAndStartPRG( prgLaunch, prgSize, true ); 
			} else
			{
				if ( radLaunchGEORAM )
				{
					// wait for "READY." to appear on screen
					WAIT_FOR_READY_PROMPT
					injectKeyInput( "SYS56832", true ); 
				} else
				if ( radSilentMode != 0xffffffff )
				{
					// wait for "READY." to appear on screen
					WAIT_FOR_READY_PROMPT
					injectMessage( true );
				}
			}

			geo.isModified = 0;

			SyncDataAndInstructionCache();
			warmCacheGeoRAM();

			// DMA remained low after inject code above 
			WAIT_FOR_CPU_HALFCYCLE
			WAIT_FOR_VIC_HALFCYCLE
			RESTART_CYCLE_COUNTER
			WAIT_UP_TO_CYCLE( 100 );
			OUT_GPIO( DMA_OUT );
			SET_GPIO( bDMA_OUT );

			geoRAMUsingPolling();

			// GUI checks reu.x
			reu.isModified = geo.isModified;
		} else
		///////////////////////////////////////////////////////////////////////
		//
		// no memory expansion (but possibly PRG start)
		//
		///////////////////////////////////////////////////////////////////////
		if ( res == RUN_MEMEXP + 3 ) // no memory expansion
		{
			extern int radSpecialBasicCommand;
			if ( radSpecialBasicCommand )
			{
				WAIT_FOR_READY_PROMPT

				setStatusMessage( &statusMsg[ 0 ], "SIDKICK CONFIGURATION" );
				setStatusMessage( &statusMsg[ 40 ], " " );
				setStatusMessage( &statusMsg[ 80 ], " " );
				injectMessage( false );

				injectKeyInput( "SYS54301", false ); 
				radSpecialBasicCommand = 0;
			} else
			{
				if ( radLaunchPRG )
				{
					WAIT_FOR_READY_PROMPT
					injectAndStartPRG( prgLaunch, prgSize, false ); 
				} else
				if ( radSilentMode != 0xffffffff )
				{
					WAIT_FOR_READY_PROMPT
					#ifdef STATUS_MESSAGES
					setStatusMessage( &statusMsg[ 0 ], "RAD DISABLED" );
					setStatusMessage( &statusMsg[ 80 ], " " );
					injectMessage( false );
					#endif
				}
			}
			goto radIsWaiting;
		} else
		///////////////////////////////////////////////////////////////////////
		//
		// VSF loading (and possibly REU emulation)
		//
		///////////////////////////////////////////////////////////////////////
		if ( res == RUN_MEMEXP + 4 ) 
		{
			// load VSF
			u32 vsfSize;
			readFile( logger, (char*)DRIVE, (char*)radImageSelectedFile, vsf, &vsfSize );
			reu.isSpecial = false;

			u8 *vsfREU = getVSFModule( vsf, vsfSize, (char *)"REU1764" );
			
			REU_SIZE_KB = 0;
			if ( vsfREU )
			{
				REU_SIZE_KB = (int)vsfREU[ VSF_SIZE_MODULE_HEADER + 0 ] + ( (int)vsfREU[ VSF_SIZE_MODULE_HEADER + 1 ] << 8 ) + ( (int)vsfREU[ VSF_SIZE_MODULE_HEADER + 2 ] << 16 ) + ( (int)vsfREU[ VSF_SIZE_MODULE_HEADER + 3 ] << 24 );
				initREU(mempool);

				// transfer register content
		        u8 *reuRegisterData = &vsfREU[ VSF_SIZE_MODULE_HEADER + 4 ];
				reu.status = reuRegisterData[ 0 ];
				reu.command = reuRegisterData[ 1 ];
				reu.shadow_addrC64 = reu.addrC64 = (u16)reuRegisterData[ 2 ] + ( (u16)reuRegisterData[ 3 ] << 8 );
				reu.shadow_addrREU = reu.addrREU = (u16)reuRegisterData[ 4 ] + ( (u16)reuRegisterData[ 5 ] << 8 );
				reu.shadow_bank = reu.bank = reuRegisterData[ 6 ];
				reu.shadow_length = reu.length = (u16)reuRegisterData[ 7 ] + ( (u16)reuRegisterData[ 8 ] << 8 );
				reu.IRQmask = reuRegisterData[ 9 ];
				reu.addrREUCtrl = reuRegisterData[ 10 ];

				// copy REU data
		        u8 *reuMem = &vsfREU[ VSF_SIZE_MODULE_HEADER + 20 ];
				memcpy( mempool, reuMem, REU_SIZE_KB * 1024 );
			}
			reu.isModified = 0;

			resetAndInjectVSF( vsf, vsfSize );

			goto radIsWaiting;
		} 

		OUT_GPIO( GAME_OUT );
		OUT_GPIO( DMA_OUT );
		OUT_GPIO_IRQ();
		SET_GPIO( bLATCH_A_OE | bIRQ_OUT | bGAME_OUT | bOE_Dx | bRW_OUT | bDMA_OUT | bDIR_Dx );
		INP_GPIO_RW();
		INP_GPIO_IRQ();

		goto hijacking;
	}
}

__attribute__((optimize("align-functions=256"))) void CRAD::FIQHandler( void *pParam )
{
}

int main( void )
{
	CRAD kernel;
	if ( kernel.Initialize() )
		kernel.Run();

	halt();
	return EXIT_HALT;
}

