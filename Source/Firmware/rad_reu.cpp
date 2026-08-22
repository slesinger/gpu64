/*

  {_______            {_          {______
        {__          {_ __               {__
        {__         {_  {__               {__
     {__           {__   {__               {__
 {______          {__     {__              {__
       {__       {__       {__            {__   
         {_________         {______________		Expansion Unit
                
 RADExp - A framework for DMA interfacing with Commodore C64/C128 computers using a Raspberry Pi Zero 2 or 3A+/3B+
        - this REU emulation reproduces the behavior of Vice's emulation (https://sourceforge.net/projects/vice-emu/)
 Copyright (c) 2022-2025 Carsten Dachsbacher <frenetic@dachsbacher.de>

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
#include "rad_reu.h"
#include "linux/kernel.h"
#include "gpu64_api.h"
#include "gpu64_vsync.h"

// gpu64: reaching CGpu64FrameBuffer::CommitFlip() from the bus-watch loop
// without including gpu64_fb.h, which pulls in Circle's framebuffer and
// character-generator headers -- the same reasoning as g_pRAD above.
boolean gpu64_commitFlip( void );

// gpu64: forward-declared rather than pulling in rad_main.h (which drags in
// the whole Circle screen/HDMI-console stack) -- see rad_main.h for the
// actual definition/assignment. Used below to reach CRAD::showTestPattern()
// when the C64 writes the gpu64 trigger register at IO2 $DF0B.
class CRAD;
extern CRAD *g_pRAD;
void gpu64_showTestPattern( CRAD *pRAD );
void gpu64_showMirror( CRAD *pRAD, const u8 *screen, const u8 *color, u8 border, u8 background );

// gpu64: set once a C64 program engages the gpu64 API -- currently that's
// just milestone 2's test-pattern trigger at $DF0B (and, since IO_ADDRESS is
// masked to 5 bits same as REU's own partial IO2 decode, really any of
// $DF0B/$DF2B/$DF4B/$DF6B/$DF8B/$DFAB/$DFCB/$DFEB -- any program that scans
// or fills across IO2, e.g. an REU-detection utility, can trip this
// incidentally). Per docs/bus_access_design.md, screen-mirror mode and
// framebuffer-API mode are mutually exclusive: once true, the periodic
// mirror snapshot in reuUsingPolling()'s main loop stops firing.
//
// Cleared in resetREU() below (called on every fresh entry into REU
// emulation) rather than never, per a real-hardware hang this caused: once
// set, it stayed set for the rest of the RPi's power-on session -- a PRG
// that genuinely tripped it once (deliberately or, per the aliasing above,
// by accident) silently disarmed the mirror for every REU session
// afterwards, with zero on-screen indication why. milestone 4's real
// command API may want its own, more deliberate "back to mirror" exit
// eventually; resetting on resetREU() is the simple fix for now.
u8 gpu64ApiActive = 0;

u32 REU_SIZE_KB = 1024;

REUSTATE reu AAA;
u8 *reuMemory;
bool reuRunning;

static u64 armCycleCounter;

static volatile u8 forceRead;

#include "lowlevel_dma.h"

void resetREU()
{
	// gpu64: see the comment on gpu64ApiActive's declaration above -- this
	// is what actually clears it now.
	gpu64ApiActive = 0;
	// gpu64: the API register file resets with it -- a fresh REU session must
	// not inherit a previous program's staged ARGs, sticky CMD_HI or error.
	gpu64_apiReset();

	reu.irqRelease = 0;

    reu.irqTriggered = 0;
    reu.reuWaitForFF00 = 0;

    reu.status = (reu.status & ~REU_STATUS_256K_CHIPS) | reu.preset;
    reu.command = REU_COMMAND_FF00_DISABLED;
    reu.length = reu.shadow_length = 0xffff;
    reu.addrC64 = 0;
    reu.addrREU = reu.shadow_addrREU = 0;//(u32)reu.regBankUnused << 16;
    reu.bank = reu.shadow_bank = reu.regBankUnused;
    reu.IRQmask = REU_INTERRUPT_UNUSED_BITMASK;
    reu.addrREUCtrl = REU_ADDR_UNUSED_BITS;

	reu.releaseDMA = 0;

	reu.contiguousWrite = 0;
	reu.contiguousVerify = 0;
	reu.contiguous1ByteWrites = 0;
}

void initializeDMATimings()
{
	reu.WAIT_FOR_SIGNALS = WAIT_FOR_SIGNALS;
	reu.WAIT_CYCLE_MULTIPLEXER = WAIT_CYCLE_MULTIPLEXER;
	reu.WAIT_CYCLE_READ = WAIT_CYCLE_READ;
	reu.WAIT_CYCLE_WRITEDATA = WAIT_CYCLE_WRITEDATA;
	reu.WAIT_CYCLE_READ2 = WAIT_CYCLE_READ + 20;
	reu.WAIT_CYCLE_READ_VIC2 = WAIT_CYCLE_READ_VIC2;
	reu.WAIT_CYCLE_WRITEDATA_VIC2 = WAIT_CYCLE_WRITEDATA_VIC2;
	reu.WAIT_CYCLE_MULTIPLEXER_VIC2 = WAIT_CYCLE_MULTIPLEXER_VIC2;
	reu.WAIT_TRIGGER_DMA = WAIT_TRIGGER_DMA;
	reu.WAIT_RELEASE_DMA = WAIT_RELEASE_DMA;
	reu.TIMING_OFFSET_CBTD = TIMING_OFFSET_CBTD;
	reu.TIMING_DATA_HOLD = TIMING_DATA_HOLD;
	reu.TIMING_TRIGGER_DMA = TIMING_TRIGGER_DMA;
	reu.TIMING_ENABLE_ADDRLATCH = TIMING_ENABLE_ADDRLATCH;
	reu.TIMING_READ_BA_WRITING = TIMING_READ_BA_WRITING;
	reu.TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING = TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING;
	reu.TIMING_ENABLE_DATA_WRITING = TIMING_ENABLE_DATA_WRITING;
	reu.TIMING_BA_SIGNAL_AVAIL = TIMING_BA_SIGNAL_AVAIL;
	reu.TIMING_RW_BEFORE_ADDR = TIMING_RW_BEFORE_ADDR;
	reu.TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING_MINUS_RW_BEFORE_ADDR = TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING - TIMING_RW_BEFORE_ADDR;

	reu.CACHING_L1_WINDOW_KB = CACHING_L1_WINDOW_KB * 1024;
	reu.CACHING_L2_OFFSET_KB = CACHING_L2_OFFSET_KB * 1024;
	reu.CACHING_L2_PRELOADS_PER_CYCLE = CACHING_L2_PRELOADS_PER_CYCLE;
}

void initREU( void *mempool )
{
	reuMemory = (u8*)mempool;

	reu.reuSize = REU_SIZE_KB * 1024;

	reu.wrapAround = 0x80000; 
    reu.wrapAroundDRAM = reu.wrapAround; // except 1700
    reu.wrapStoring = reu.wrapAround - 1;

    reu.regBankUnused = REU_BANK_UNUSED_BITS;
	reu.preset = REU_STATUS_256K_CHIPS;

	switch ( REU_SIZE_KB )
	{
	case 128:
		reu.preset = 0;
		reu.wrapAround = 
		reu.wrapAroundDRAM = 0x20000; 
		break;
	case 256:
	case 512:
		break;
	default:
        reu.regBankUnused = 0;
	    reu.wrapAroundDRAM = reu.reuSize;
	    reu.wrapStoring = reu.reuSize - 1;
		break;
	}

	resetREU();
	initializeDMATimings();
}


__attribute__( ( always_inline ) ) inline void reuUpdateRegisters( u16 host_addr, u32 reu_addr, int len, u8 new_status_or_mask )
{
    reu_addr &= reu.wrapStoring;

    reu.status |= new_status_or_mask;

    if ( !(reu.command & REU_COMMAND_AUTOLOAD)) 
    {
        // no autoload
		if ( BITS_ALL_CLR( reu.addrREUCtrl, REU_ADDR_FIX_C64 ) )
			reu.addrC64 = host_addr;

		if ( BITS_ALL_CLR( reu.addrREUCtrl, REU_ADDR_FIX_REU ) )
		{
			reu.addrREU = reu_addr & 0xffff;
			reu.bank = ( reu_addr >> 16 ) & 0xff;
		}

        reu.length = len & 0xFFFF;
    } else 
    {
        reu.addrC64 = reu.shadow_addrC64;
        reu.addrREU = reu.shadow_addrREU;
		reu.bank = reu.shadow_bank;
        reu.length = reu.shadow_length;
    }

	if ( BITS_ALL_SET( new_status_or_mask, REU_STATUS_END_OF_BLOCK ) )
	{
		// check for interrupt, if no verify error
		if ( BITS_ALL_SET( reu.IRQmask, REU_INTERRUPT_END_BLOCK | REU_INTERRUPTS_ENABLED ) )
		{
			reu.status |= REU_STATUS_INTERRUPT_PENDING;
			reu.irqTriggered = 1;
		}
	}

	if ( BITS_ALL_SET( new_status_or_mask, REU_STATUS_VERIFY_ERROR ) )
	{
		if ( BITS_ALL_SET( reu.IRQmask, REU_INTERRUPT_VERIFY | REU_INTERRUPTS_ENABLED ) )
		{
			reu.status |= REU_STATUS_INTERRUPT_PENDING;
			reu.irqTriggered = 1;
		}
	}
}

#define REU_GET_INCREMENT   (1 - ( ( reu.addrREUCtrl >> 6 ) & 1 ))
#define REU_GET_C64INCREMENT   (1 - ( ( reu.addrREUCtrl >> 7 ) & 1 ))

__attribute__( ( always_inline ) ) inline void REU_INCREMENT_ADDRESS( u32 &r_a )
{
    u32 next = ( r_a & 0x0007ffff) + reu.incrREU;

    if (next == reu.wrapAround) 
        next = 0;
    
    r_a = (r_a & 0x00f80000) | next;
}

__attribute__( ( always_inline ) ) inline u32 REU_GET_NEXT_ADDRESS( u32 r_a )
{
    u32 next = ( r_a & 0x0007ffff) + reu.incrREU;
    if (next == reu.wrapAround) 
        next = 0;
    return (r_a & 0x00f80000) | next;
}

#define REU_INCREMENT_C64ADDRESS( a ) { a = ( a + reu.incrC64 ) & 0xffff; }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"

__attribute__( ( always_inline ) ) inline void reuStore( u32 reu_addr, u8 value )
{
    reu_addr &= reu.wrapAroundDRAM - 1;
    if (reu_addr < reu.reuSize ) 
        reuMemory[reu_addr] = value;
}
#pragma GCC diagnostic pop

__attribute__( ( always_inline ) ) inline u8 reuLoad( u32 reu_addr )
{
    reu_addr &= reu.wrapAroundDRAM - 1;
	if ( reu_addr < reu.reuSize )
		return reuMemory[ reu_addr ];
    return 0xff;
}

__attribute__( ( always_inline ) ) inline u8 reuLoad32( u32 reu_addr )
{
	reu_addr &= reu.wrapAroundDRAM - 1;
	if ( reu_addr < reu.reuSize )
		return *(u32*)&reuMemory[ reu_addr ];
    return 0xff;
}

__attribute__( ( always_inline ) ) inline void reuPrefetch( u32 reu_addr )
{
	CACHE_PRELOAD_REU( &reuMemory[ ( reu_addr&~63 ) & ( reu.wrapAroundDRAM - 1 ) ] );
}

__attribute__( ( always_inline ) ) inline void reuPrefetchL1( u32 reu_addr )
{
	CACHE_PRELOADL1STRM( &reuMemory[ ( reu_addr&~63 ) & ( reu.wrapAroundDRAM - 1 ) ] );
}

__attribute__( ( always_inline ) ) inline void reuPrefetchW( u32 reu_addr )
{
	CACHE_PRELOADL1STRMW( &reuMemory[ ( reu_addr&~63 ) & ( reu.wrapAroundDRAM - 1 ) ] );
}


// gpu64: milestone 3 default-state screen mirror -- global rather than
// stack-local since reuUsingPolling() below is a cycle-critical,
// register-heavy function and these are too big to want living on its stack.
static u8 gpu64MirrorScreen[ 1000 ];
static u8 gpu64MirrorColor[ 1000 ];
static u8 gpu64MirrorBorder = 0, gpu64MirrorBackground = 0;

// gpu64: how often (in reuUsingPolling() main-loop passes, roughly one C64
// CPU cycle each) to grab a mirror snapshot. 1000000 was the original guess
// and measured ~1fps on real hardware (RAD cartridge + RPi 3A+) -- too slow,
// per hw feedback. Scaled down from that real calibration point for ~4fps;
// the fps-per-count ratio itself is still only measured at the one data
// point above, so this is an extrapolation, not a second hardware
// measurement -- worth re-checking on hardware rather than assumed exact.
#define GPU64_MIRROR_POLL_INTERVAL 250000

// gpu64: milestone 3 screen mirror. Grabs a brief DMA burst -- the same
// CLR_GPIO(bDMA_OUT)/DMA_READBYTE_P1..P3 cycle-stealing technique REU's own
// Store/Fetch transfers already use in handle_transfer.h, not a new kind of
// bus takeover -- to read the default screen RAM ($0400-$07E7) and color RAM
// ($D800-$DBE7), plus the current border/background color ($D020/$D021),
// then hands them to CRAD::showMirror() (rad_main.cpp) for rendering.
// Scoped per docs/bus_access_design.md: fixed default addresses only (no VIC
// bank / $D018 detection yet -- a program that relocates its screen will
// render wrong until that's added), standard text mode only, and gpu64's own
// bundled character ROM copy (font_bin, see rad_main.cpp) rather than
// peeking the C64's real char ROM -- which the CPU can't see when it's
// banked out anyway, and font_bin is only ever mutated by the menu's logo
// code during the earlier hijack/menu phase, not while this runs.
//
// __attribute__ set to match reuUsingPolling() below (same file, same
// reasoning: this file's cache-preloading/alignment choices are load-bearing
// for cycle-precise timing, see the comment above reuLoad32() usage in
// reuUsingPolling()'s tail). Needs its own instruction-cache preload before
// first use -- see warmCache() in rad_main.cpp -- since it's called from
// inside reuUsingPolling() but isn't covered by that function's own preload
// window.
__attribute__( ( optimize( "align-functions=256" ) ) )
__attribute__( ( section( "section_polling" ) ) )
void gpu64_mirrorSnapshot()
{
	register u32 g2;
	register u16 c_a;
	register u8 x;

	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER
	WAIT_UP_TO_CYCLE( reu.TIMING_TRIGGER_DMA );
	CLR_GPIO( bDMA_OUT );

	for ( u16 i = 0; i < 1000; i++ )
	{
		c_a = 0x0400 + i;
		DMA_READBYTE_P1( c_a );
		DMA_READBYTE_P2();
		DMA_READBYTE_P3( x, false );
		gpu64MirrorScreen[ i ] = x;
	}

	for ( u16 i = 0; i < 1000; i++ )
	{
		c_a = 0xD800 + i;
		DMA_READBYTE_P1( c_a );
		DMA_READBYTE_P2();
		DMA_READBYTE_P3( x, false );
		gpu64MirrorColor[ i ] = x & 0x0F;
	}

	c_a = 0xD020;
	DMA_READBYTE_P1( c_a );
	DMA_READBYTE_P2();
	DMA_READBYTE_P3( x, false );
	gpu64MirrorBorder = x & 0x0F;

	c_a = 0xD021;
	DMA_READBYTE_P1( c_a );
	DMA_READBYTE_P2();
	DMA_READBYTE_P3( x, true );		// last byte: release DMA back to the C64
	gpu64MirrorBackground = x & 0x0F;

	gpu64_showMirror( g_pRAD, gpu64MirrorScreen, gpu64MirrorColor, gpu64MirrorBorder, gpu64MirrorBackground );
}

// gpu64: commits a vblank-deferred PAGE_FLIP, at the frame boundary the C64
// asked for. Everything else a boundary does is inlined into the loop
// (gpu64_vsyncAdvance(), gpu64_vsync.h) precisely so this is the only call
// on the path -- and this one only happens when a flip is actually pending.
//
// The commit is bracketed in a DMA hold, exactly like the command dispatch
// and the mirror snapshot. SetVirtualOffset() is a mailbox round-trip to the
// VideoCore whose cost nobody has measured (it was open question 1 in
// docs/milestone4_2d_api_design.md), and this runs while the C64 is
// free-running through the loop -- so without the hold, a slow mailbox call
// would silently eat the C64's next IO2 access, which is milestone 4's
// bug #2 all over again. With the hold, the cost stops mattering for
// correctness and only makes the burst longer.
//
// This function's instruction cache is warmed by gpu64_vsyncWarmCommit()
// below, called from the PAGE_FLIP dispatch that queues the flip -- i.e.
// while the bus is already held, rather than here where warming would itself
// be the unprotected delay it is meant to prevent. Same __attribute__ pair
// as gpu64_mirrorSnapshot() above.
__attribute__( ( optimize( "align-functions=256" ) ) )
__attribute__( ( section( "section_polling" ) ) )
void gpu64_vsyncCommitFlip( void )
{
	// The WAIT_FOR_*_HALFCYCLE macros sample the GPIO bank into a variable
	// they expect to be called g2, same as gpu64_mirrorSnapshot() above.
	register u32 g2;
	(void)g2;

	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER
	WAIT_UP_TO_CYCLE( reu.TIMING_TRIGGER_DMA );
	CLR_GPIO( bDMA_OUT );

	gpu64_commitFlip();

	WAIT_FOR_CPU_HALFCYCLE
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER
	SET_GPIO( bDMA_OUT );

	gpu64Vsync.flipPending = 0;
	gpu64Regs.status &= ~GPU64_STATUS_BUSY;
}

// gpu64: called from the PAGE_FLIP dispatch, with the bus held, to warm the
// commit path before the loop needs it. The dispatch that queues a flip is
// the biggest instruction-cache consumer in the system and will have evicted
// whatever warmCache() preloaded at REU start -- see the general rule in
// docs/progress_tracker.md. Doing it here rather than at the point of use is
// what keeps the loop's own exposure to a single short call.
void gpu64_vsyncWarmCommit( void )
{
	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)gpu64_vsyncCommitFlip, 1024 * 2 );
	FORCE_READ_LINEARa( (void*)gpu64_vsyncCommitFlip, 1024 * 2, 1024 * 2 );
}

// gpu64: milestone 4 blob transfers -- the "commands by reference" half of
// the API (docs/api_design.md). A C64-space transfer is the same brief,
// bounded DMA burst gpu64_mirrorSnapshot() above already proves on hardware,
// just parameterized instead of hardcoded to screen RAM; an REU-space
// transfer never touches the bus at all, since REU memory is ours.
//
// Same __attribute__s and the same cache-preload requirement as
// gpu64_mirrorSnapshot() -- see warmCache() in rad_main.cpp.
__attribute__( ( optimize( "align-functions=256" ) ) )
__attribute__( ( section( "section_polling" ) ) )
u8 gpu64_blobRead( u8 space, u32 addr, u32 len, u8 *pDst )
{
	if ( len == 0 )
		return GPU64_ERR_OK;

	if ( space == GPU64_SPACE_REU )
	{
		if ( addr + len > reu.reuSize )
			return GPU64_ERR_OUT_OF_RANGE;
		memcpy( pDst, &reuMemory[ addr ], len );
		return GPU64_ERR_OK;
	}

	if ( space != GPU64_SPACE_C64 )
		return GPU64_ERR_OUT_OF_RANGE;

	if ( addr + len > 65536 )
		return GPU64_ERR_OUT_OF_RANGE;

	// The bus is already held: this is only ever reached from a CMD_LO
	// dispatch, which asserts DMA around the whole command (see the write
	// handler in reuUsingPolling()). Sync to a cycle boundary and go.
	register u32 g2;
	register u16 c_a;
	register u8 x;

	// gpu64: warm this function's own instruction cache immediately before
	// the burst, not just once in warmCache(). The dispatch that got us here
	// runs a lot of code first -- a CLEAR alone walks 64000 framebuffer
	// bytes and cleans them -- which is more than enough to evict what
	// warmCache() preloaded at REU start. The observed symptom was exact:
	// the first launch of a program lost the first two bytes of its first
	// blob read, every subsequent RUN was clean. This is the same
	// preload-then-force-read pair warmCache() uses, just at the point where
	// it is actually needed. Free in bus terms: the C64 is DMA-halted here,
	// so nothing is racing us.
	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)gpu64_blobRead, 1024 * 2 );
	FORCE_READ_LINEARa( (void*)gpu64_blobRead, 1024 * 2, 1024 * 2 );
	// ...and the first stretch of the destination, so the very first store
	// of the burst isn't a cache miss either.
	// nWarm in a local, not inline: these are unparenthesised macro
	// arguments, so a ternary here gets torn apart by operator precedence.
	// Passed as `acc` to FORCE_READ_LINEARa below it produced `i < len <
	// 1024 ? len : 1024` -- a loop condition that is always true, which hung
	// the firmware inside a DMA hold and left the C64 halted forever.
	const u32 nWarm = len < 1024 ? len : 1024;
	CACHE_PRELOAD_DATA_CACHE( pDst, nWarm, CACHE_PRELOADL1KEEP )

	// gpu64: WAIT_FOR_CPU_HALFCYCLE first, so this catches the *transition*
	// into the VIC half-cycle. WAIT_FOR_VIC_HALFCYCLE alone returns
	// immediately if we are already in one -- and we get here at whatever
	// phase the dispatcher's argument decoding happened to finish, unlike
	// handle_transfer.h, which enters straight off the loop's own sync. The
	// symptom of getting this wrong is subtle: DMA_READBYTE_P2's
	// WAIT_FOR_CPU_HALFCYCLE re-syncs the burst, so only the first byte or
	// two of a transfer come back wrong, at random.
	WAIT_FOR_CPU_HALFCYCLE
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER
	WAIT_UP_TO_CYCLE( reu.TIMING_TRIGGER_DMA );

	for ( u32 i = 0; i < len; i++ )
	{
		c_a = (u16)( addr + i );
		DMA_READBYTE_P1( c_a );
		DMA_READBYTE_P2();
		DMA_READBYTE_P3( x, false );			// never releases: dispatch owns the hold
		pDst[ i ] = x;
	}

	return GPU64_ERR_OK;
}

__attribute__( ( optimize( "align-functions=256" ) ) )
__attribute__( ( section( "section_polling" ) ) )
u8 gpu64_blobWrite( u8 space, u32 addr, u32 len, const u8 *pSrc )
{
	if ( len == 0 )
		return GPU64_ERR_OK;

	if ( space == GPU64_SPACE_REU )
	{
		if ( addr + len > reu.reuSize )
			return GPU64_ERR_OUT_OF_RANGE;
		memcpy( &reuMemory[ addr ], pSrc, len );
		return GPU64_ERR_OK;
	}

	if ( space != GPU64_SPACE_C64 )
		return GPU64_ERR_OUT_OF_RANGE;

	if ( addr + len > 65536 )
		return GPU64_ERR_OUT_OF_RANGE;

	// Write direction: same cycle-stealing burst as the read above, using the
	// macros REU's own Stash transfer uses (handle_transfer.h). This is the
	// first time gpu64 pushes data *back* to the C64 rather than reading it.
	// Bus already held by the dispatch wrapper, same as gpu64_blobRead().
	register u32 g2;
	register u16 c_a;

	// gpu64: warm this function's own instruction cache immediately before
	// the burst, not just once in warmCache(). The dispatch that got us here
	// runs a lot of code first -- a CLEAR alone walks 64000 framebuffer
	// bytes and cleans them -- which is more than enough to evict what
	// warmCache() preloaded at REU start. The observed symptom was exact:
	// the first launch of a program lost the first two bytes of its first
	// blob read, every subsequent RUN was clean. This is the same
	// preload-then-force-read pair warmCache() uses, just at the point where
	// it is actually needed. Free in bus terms: the C64 is DMA-halted here,
	// so nothing is racing us.
	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)gpu64_blobWrite, 1024 * 2 );
	FORCE_READ_LINEARa( (void*)gpu64_blobWrite, 1024 * 2, 1024 * 2 );
	const u32 nWarm = len < 1024 ? len : 1024;		// see gpu64_blobRead()
	FORCE_READ_LINEARa( pSrc, len, nWarm );

	// gpu64: WAIT_FOR_CPU_HALFCYCLE first, so this catches the *transition*
	// into the VIC half-cycle. WAIT_FOR_VIC_HALFCYCLE alone returns
	// immediately if we are already in one -- and we get here at whatever
	// phase the dispatcher's argument decoding happened to finish, unlike
	// handle_transfer.h, which enters straight off the loop's own sync. The
	// symptom of getting this wrong is subtle: DMA_READBYTE_P2's
	// WAIT_FOR_CPU_HALFCYCLE re-syncs the burst, so only the first byte or
	// two of a transfer come back wrong, at random.
	WAIT_FOR_CPU_HALFCYCLE
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER
	WAIT_UP_TO_CYCLE( reu.TIMING_TRIGGER_DMA );

	for ( u32 i = 0; i < len; i++ )
	{
		c_a = (u16)( addr + i );
		DMA_WRITEBYTE_P1( c_a, pSrc[ i ] );
		DMA_WRITEBYTE_P2( false );			// never releases: dispatch owns the hold
	}

	return GPU64_ERR_OK;
}

#if 1
__attribute__( ( optimize( "align-functions=256" ) ) )
__attribute__( ( section( "section_polling" ) ) )
u8 reuUsingPolling( int step )
{
	register u32 g2 = bBUTTON, g3;
	register u16 resetCount = 0;
	// gpu64: counts main-loop passes toward the next mirror snapshot -- see
	// gpu64_mirrorSnapshot() and GPU64_MIRROR_POLL_INTERVAL above.
	register u32 gpu64MirrorPollCounter = 0;

	u16 ipl = 0;

	if ( step <= 1 )
	{
		void *p = ( && reuEmulationMainLoop );
		CACHE_PRELOAD_INSTRUCTION_CACHE( p, 0x1a00 );

		if ( step == 1 ) return 0;

		for ( u16 i = 0; i < 20000; i++ )
		{
			WAIT_FOR_CPU_HALFCYCLE
			WAIT_FOR_VIC_HALFCYCLE
			RESTART_CYCLE_COUNTER
		}
		SET_GPIO( bDMA_OUT );
	}

reuEmulationMainLoop:

	CLR_GPIO( bMPLEX_SEL );
	WAIT_FOR_CPU_HALFCYCLE
	BEGIN_CYCLE_COUNTER

	while ( 1 )
	{
		WAIT_FOR_VIC_HALFCYCLE

		void *p = (u8*)( && reuEmulationMainLoop ) + ipl;
		CACHE_PRELOADIKEEP( p );
		ipl += 64; if ( ipl >= 0x1a00 ) ipl = 0;

		if ( CPU_RESET )
		{
			resetCount ++;
			if ( resetCount > 1000 )
				resetREU();
		} else
			resetCount = 0;

		if ( BUTTON_PRESSED )
			return 2;

		// gpu64: default-state screen mirror (milestone 3) -- time-based, not
		// IO2-access-based, so this fires independent of whatever the C64
		// program is doing this cycle. Stops entirely once a program engages
		// the gpu64 API (see gpu64ApiActive above).
		if ( !gpu64ApiActive && ++gpu64MirrorPollCounter >= GPU64_MIRROR_POLL_INTERVAL )
		{
			gpu64MirrorPollCounter = 0;
			gpu64_mirrorSnapshot();
		}

		// gpu64: the frame clock (gpu64_vsync.h). Only meaningful once a
		// program has engaged the API -- the mirror above has no use for a
		// vblank and shares the loop pass with this. The common case is the
		// inlined test alone: one MMIO read and a compare.
		if ( gpu64ApiActive )
		{
			register u32 gpu64Now = gpu64_vsyncNow();
			if ( gpu64_vsyncDue( gpu64Now ) )
			{
				gpu64_vsyncAdvance( gpu64Now );
				gpu64Regs.status |= GPU64_STATUS_VBLANK_PENDING;
				if ( gpu64Vsync.flipPending )
					gpu64_vsyncCommitFlip();
			}
		}

		SET_GPIO( bDIR_Dx );
		WAIT_FOR_CPU_HALFCYCLE
		RESTART_CYCLE_COUNTER
		WAIT_UP_TO_CYCLE( reu.WAIT_FOR_SIGNALS + reu.TIMING_OFFSET_CBTD );
		g2 = read32( ARM_GPIO_GPLEV0 );

		SET_GPIO( bMPLEX_SEL );

		WAIT_UP_TO_CYCLE( reu.WAIT_CYCLE_MULTIPLEXER );
		g3 = read32( ARM_GPIO_GPLEV0 );
		CLR_GPIO( bMPLEX_SEL );

		register u8 D = 0;
		register u8 writeFF00 = 0;

		if ( !IO2_ACCESS && !ADDRESS_FFxx )
			goto noREUAccess;

		if ( CPU_WRITES_TO_BUS )
		{
			if ( ADDRESS_FFxx && ADDRESS0to7 == 0 && reu.reuWaitForFF00 )
			{
				writeFF00 = 1;
			} else
			if ( IO2_ACCESS )
			{
				SET_BANK2_INPUT
				///SET_GPIO( bDIR_Dx );
				CLR_GPIO( bOE_Dx );				// Dx = enable
				WAIT_UP_TO_CYCLE( reu.WAIT_CYCLE_WRITEDATA );
				D = ( read32( ARM_GPIO_GPLEV0 ) >> D0 ) & 255;
				SET_GPIO( bOE_Dx );				// Dx = disable
				SET_BANK2_OUTPUT
			}

			// gpu64: full 8-bit decode, not REU's own "& 0x1f" -- $DF0B-$DFFF
			// is gpu64's register window (docs/api_design.md), and the 5-bit
			// mask would alias it back onto REU's 11 registers.
			register u8 addr = IO_ADDRESS;

			if ( ( IO2_ACCESS && addr == 0x01 && BITS_ALL_SET( D, REU_COMMAND_EXECUTE | REU_COMMAND_FF00_DISABLED ) )
					|| writeFF00 )
			{
				if ( !writeFF00 ) reu.command = D;
				#include "handle_transfer.h"
				reu.reuWaitForFF00 = 0;
			} else
			if ( IO2_ACCESS  )
			{
				register u8 addr = IO_ADDRESS;

				// gpu64: $DF0B-$DFFF is gpu64's own register window -- the
				// command API's registers (docs/api_design.md). REU keeps
				// $DF00-$DF0A below. Note this is the full 8-bit address:
				// REU's own "& 0x1f" decode would alias the whole gpu64
				// window down onto REU's 11 registers.
				//
				// Everything the API does happens synchronously here, in the
				// same immediate-mode model milestone 2's test pattern and
				// milestone 3's mirror already use on real hardware. The C64
				// free-runs through this loop (see the "Operating modes"
				// section of docs/project_description.md); a command that
				// moves a payload steals the bus for one bounded burst,
				// proportional to a length the C64 itself chose.
				if ( addr >= GPU64_REG_CMD_HI )
				{
					if ( addr == GPU64_REG_CMD_LO )
					{
						// gpu64: hold the bus for the whole command.
						//
						// Found on the first hardware test: staging ARG
						// bytes and then writing CMD_LO only works if the
						// C64 cannot issue a write while gpu64 is executing
						// the previous one. It can -- the C64 free-runs
						// through this loop -- and a CLEAR is ~64000 byte
						// writes plus a cache clean, during which the CPU
						// gets through dozens of instructions whose IO2
						// writes this loop never sees. The symptom was a
						// command that vanished entirely and a following one
						// drawn from half-stale arguments.
						//
						// So dispatch runs with the CPU DMA-halted, exactly
						// like an REU transfer: bounded by the command's own
						// work, and the C64 resumes at its next instruction
						// with nothing missed. Blob transfers inside the
						// command no longer release the bus themselves --
						// the release below is the only one.
						WAIT_FOR_VIC_HALFCYCLE
						RESTART_CYCLE_COUNTER
						WAIT_UP_TO_CYCLE( reu.TIMING_TRIGGER_DMA );
						CLR_GPIO( bDMA_OUT );

						gpu64_apiDispatch( D );

						// gpu64: give the bus back on a cycle boundary, not
						// wherever the command happened to finish. Every
						// other release in RAD (DMA_READBYTE_P3,
						// DMA_WRITEBYTE_P2 via
						// DISABLE_ADDRESS_LATCH_AND_BUSTRANSCEIVER) syncs to
						// the start of a VIC half-cycle first; releasing
						// mid-cycle restarts the 6502 at an undefined phase,
						// which is how a program survives the first command
						// or two and then quietly derails.
						WAIT_FOR_CPU_HALFCYCLE
						WAIT_FOR_VIC_HALFCYCLE
						RESTART_CYCLE_COUNTER
						SET_GPIO( bDMA_OUT );
					} else
						// Inlined (gpu64_api.h): a cross-TU call here has to
						// finish before the loop's next sample, and an
						// i-cache miss on it loses the C64's next IO2 access
						// outright -- which showed up as commands running on
						// half-written argument blocks.
						gpu64_apiWriteReg( addr, D );
				} else
				{
					switch ( addr )
					{
					case 0x00:
						break;
					case 0x01:
						reu.command = D;

						if ( ( D & REU_COMMAND_EXECUTE ) && !( D & REU_COMMAND_FF00_DISABLED ) )
							reu.reuWaitForFF00 = 1;
						break;
					case 0x02:
						reu.addrC64 = reu.shadow_addrC64 = ( reu.shadow_addrC64 & 0xff00 ) | D;
						break;
					case 0x03:
						reu.addrC64 = reu.shadow_addrC64 = ( reu.shadow_addrC64 & 0x00ff ) | ( D << 8 );
						break;
					case 0x04:
						reu.addrREU = reu.shadow_addrREU = ( reu.shadow_addrREU & 0xff00 ) | D;
						reu.pl = reu.CACHING_L2_OFFSET_KB; reu.pl2 = 0;
						break;
					case 0x05:
						reu.addrREU = reu.shadow_addrREU = ( reu.shadow_addrREU & 0x00ff ) | ( D << 8 );
						reu.pl = reu.CACHING_L2_OFFSET_KB; reu.pl2 = 0;
						break;
					case 0x06:
						reu.bank = reu.shadow_bank = D & ~reu.regBankUnused;
						reu.pl = reu.CACHING_L2_OFFSET_KB; reu.pl2 = 0;
						break;
					case 0x07:
						reu.length = reu.shadow_length = ( reu.shadow_length & 0xff00 ) | D;
						reu.pl = reu.CACHING_L2_OFFSET_KB; reu.pl2 = 0;
						break;
					case 0x08:
						reu.length = reu.shadow_length = ( reu.shadow_length & 0x00ff ) | ( D << 8 );
						reu.pl = reu.CACHING_L2_OFFSET_KB; reu.pl2 = 0;
						break;
					case 0x09:
						reu.IRQmask = D | REU_INTERRUPT_UNUSED_BITMASK;
						if ( BITS_ALL_SET( reu.IRQmask, REU_INTERRUPT_END_BLOCK | REU_INTERRUPTS_ENABLED ) &&
								BITS_ALL_SET( reu.status, REU_STATUS_END_OF_BLOCK ) )
						{
							reu.status |= REU_STATUS_INTERRUPT_PENDING;
							reu.irqTriggered = 1;
						}
						if ( BITS_ALL_SET( reu.IRQmask, REU_INTERRUPT_VERIFY | REU_INTERRUPTS_ENABLED ) &&
								BITS_ALL_SET( reu.status, REU_STATUS_VERIFY_ERROR ) )
						{
							reu.status |= REU_STATUS_INTERRUPT_PENDING;
							reu.irqTriggered = 1;
						}
						break;
					case 0x0A:
						reu.addrREUCtrl = D | REU_ADDR_UNUSED_BITS;
						break;
					}
					reuPrefetch( reu.addrREU | ( (u32)reu.bank << 16 ) );
				}
			}
		} else
			// CPU READS FROM BUS
			if ( IO2_ACCESS )
			{
				// gpu64: full 8-bit decode again. This also fixes a real
				// landmine on the read path: with the old 5-bit mask, a read
				// from anywhere in gpu64's window indexed
				// ((u8*)&reu.status)[addr] with addr up to 0x1f, walking off
				// the end of REUSTATE and returning whatever happened to be
				// there. gpu64's registers get their own bounds-checked read
				// (gpu64_apiReadReg), and REU's indexing now only ever sees
				// 0x00-0x0A.
				register u8 addr = IO_ADDRESS;

				register u8 disableIRQ = 0;
				// this is how this looks like in readable form:
				/*				switch ( addr )
							{
							case 0x00:	D = reu.status;
										reu.status &= ~( REU_STATUS_VERIFY_ERROR | REU_STATUS_END_OF_BLOCK | REU_STATUS_INTERRUPT_PENDING );
										disableIRQ = 1;
										break;
							case 0x01:	D = reu.command;							break;
							case 0x02:	D = reu.addrC64 & 255;					break;
							case 0x03:	D = reu.addrC64 >> 8;					break;
							case 0x04:	D = reu.addrREU & 255;						break;
							case 0x05:	D = ( reu.addrREU >> 8 ) & 255;				break;
							case 0x06:	D = reu.bank | reu.regBankUnused;		break;
							case 0x07:	D = reu.length & 255;					break;
							case 0x08:	D = reu.length >> 8;						break;
							case 0x09:	D = reu.IRQmask;							break;
							case 0x0A:	D = reu.addrREUCtrl;						break;
							default:	D = 0xFF;									break;
							}*/
				if ( addr >= GPU64_REG_CMD_HI )
				{
					// Inlined (gpu64_api.h) -- this runs against a hard
					// deadline: D has to be on the bus by WAIT_CYCLE_READ2
					// below, so a call with a cold i-cache means the C64
					// latches whatever was on the bus instead of the
					// register. That was the intermittently wrong ERRCODE.
					D = gpu64_apiReadReg( addr );
				} else
				{
					D = ( (u8 *)&reu.status )[ addr ];
					if ( addr == 0 )
					{
						reu.status &= ~( REU_STATUS_VERIFY_ERROR | REU_STATUS_END_OF_BLOCK | REU_STATUS_INTERRUPT_PENDING );
						disableIRQ = 1;
					} else
						if ( addr == 6 )
						{
							D |= reu.regBankUnused;
						}
				}

				register u32 DD = D << D0;
				write32( ARM_GPIO_GPCLR0, ( D_FLAG & ( ~DD ) ) | bOE_Dx | bDIR_Dx );
				write32( ARM_GPIO_GPSET0, DD );
				SET_BANK2_OUTPUT

					if ( disableIRQ && reu.irqRelease )
					{
						reu.irqRelease = 0;
						write32( ARM_GPIO_GPSET0, bIRQ_OUT );
						INP_GPIO_IRQ();
					}

				WAIT_UP_TO_CYCLE( reu.WAIT_CYCLE_READ2 );
				SET_GPIO( bOE_Dx | bDIR_Dx );
			}

	noREUAccess:
		if ( reu.irqTriggered && !CPU_IRQ_LOW )
		{
			reu.irqTriggered = 0;
			reu.irqRelease = 1;
			write32( ARM_GPIO_GPCLR0, bIRQ_OUT );
			OUT_GPIO_IRQ();
		}

		// gpu64: the vblank IRQ, shaped exactly like REU's above -- same
		// physical line, same "only assert when nobody else is pulling it
		// low" guard. The difference is the release: REU lets go when the
		// C64 reads the register that explains the interrupt, whereas
		// gpu64's is explicit, on VBLANK_ACK (see doSystem() $03), because
		// nothing gpu64 exposes is read-to-clear.
		//
		// Gated on gpu64ApiActive, like the frame clock at the top of the
		// loop: nothing here can have anything to do until a program has
		// engaged the API, and the loop's tail is the part that has to be
		// finished before the next WAIT_FOR_VIC_HALFCYCLE. One byte test
		// (and gpu64ApiActive is already hot -- the mirror above reads it
		// every pass) beats two struct loads on every pass of a loop that
		// runs a million times a second while nothing is happening.
		if ( gpu64ApiActive && gpu64Vsync.irqRequest && !CPU_IRQ_LOW )
		{
			gpu64Vsync.irqRequest = 0;
			gpu64Vsync.irqAsserted = 1;
			write32( ARM_GPIO_GPCLR0, bIRQ_OUT );
			OUT_GPIO_IRQ();
		}

		if ( gpu64ApiActive && gpu64Vsync.irqReleaseReq )
		{
			gpu64Vsync.irqReleaseReq = 0;
			gpu64Vsync.irqAsserted = 0;
			// Only drive the pin back up if REU is not also holding it
			// down; if it is, REU's own release path owns the line and
			// letting go here would cancel an interrupt the C64 has not
			// acknowledged yet.
			if ( !reu.irqRelease )
			{
				write32( ARM_GPIO_GPSET0, bIRQ_OUT );
				INP_GPIO_IRQ();
			}
		}

		// cache preloading is the most crucial part of emulating a REU on a RPi
		// changing anything below might make everything less stable
		for ( int i = 0; i < reu.CACHING_L2_PRELOADS_PER_CYCLE; i++ )
		{
			reuPrefetch( ( reu.addrREU | ( (u32)reu.bank << 16 ) ) + reu.pl );
			reu.pl += 64;
			if ( reu.pl >= reu.length + 64 ) reu.pl = 0;
		}

		CACHE_PRELOADL1STRM( &reuMemory[ ( ( ( reu.pl2 + reu.addrREU ) | ( (u32)reu.bank << 16 ) ) & ~63 ) & ( reu.reuSize - 1 ) ] );
		reu.pl2 += 64; if ( reu.pl2 >= min( reu.CACHING_L1_WINDOW_KB - 64, reu.length ) ) reu.pl2 = 0;

		forceRead = reuLoad32( 0 );
	}
}
#endif