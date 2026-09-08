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
#include "gpu64_3d.h"
#include "gpu64_ladder.h"
#include "gpu64_holdgap.h"
#include "gpu64_holdgate.h"

// gpu64: reaching CGpu64FrameBuffer::CommitFlip() from the bus-watch loop
// without including gpu64_fb.h, which pulls in Circle's framebuffer and
// character-generator headers -- the same reasoning as g_pRAD above.
boolean gpu64_commitFlip( void );
// gpu64: warms the mailbox post the commit ends in -- see gpu64_flip.h.
void gpu64_flipWarm( void );

// gpu64: forward-declared rather than pulling in rad_main.h (which drags in
// the whole Circle screen/HDMI-console stack) -- see rad_main.h for the
// actual definition/assignment. Used below to reach CRAD::showTestPattern()
// when the C64 writes the gpu64 trigger register at IO2 $DF0B.
class CRAD;
extern CRAD *g_pRAD;
void gpu64_showTestPattern( CRAD *pRAD );
void gpu64_showMirror( CRAD *pRAD, const u8 *screen, const u8 *color, u8 border, u8 background, u8 d018 );

// gpu64: set once a C64 program engages the gpu64 API -- currently that's
// just milestone 2's test-pattern trigger at $DF0B (and, since IO_ADDRESS is
// masked to 5 bits same as REU's own partial IO2 decode, really any of
// $DF0B/$DF2B/$DF4B/$DF6B/$DF8B/$DFAB/$DFCB/$DFEB -- any program that scans
// or fills across IO2, e.g. an REU-detection utility, can trip this
// incidentally). Per project/bus_access_design.md, screen-mirror mode and
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

// gpu64: mirror liveness state (2026-09-06 rework -- see the long comment on
// gpu64_mirrorSnapshot() below for the whole design).
//
// File scope rather than reuUsingPolling() locals for two reasons: resetREU()
// has to clear them and cannot reach that function's registers, and the
// per-pass test at the top of the loop wants them on the same already-hot
// cache line as gpu64ApiActive above.
//
// enabled       -- the latch. Cleared the moment the machine stops being "a
//                  BASIC prompt with nothing running"; only resetREU(), i.e.
//                  a C64 reset, ever sets it again.
// seenPrompt    -- has this session ever observed the direct-mode prompt?
//                  Until it has, the CURLIN test below is disarmed, because
//                  the KERNAL's power-on RAM test runs for ~2s with $3A
//                  holding uninitialised garbage and would latch the mirror
//                  off before BASIC ever came up.
// irqWatchdog   -- main-loop passes (~1 C64 cycle each) since the last jiffy
//                  IRQ vector fetch was seen on the bus.
// awaitIrq      -- set by every re-arm, cleared by the first vector fetch
//                  after it. The watchdog does not run while this is set,
//                  because a freshly reset C64 does not have a jiffy IRQ yet
//                  and takes far longer to get one than the watchdog allows.
//                  See GPU64_MIRROR_IRQ_TIMEOUT below.
// jiffy         -- vector fetches counted toward the next snapshot.
u8 gpu64MirrorEnabled = 1;
static u8 gpu64MirrorSeenPrompt = 0;
static u32 gpu64MirrorIrqWatchdog = 0;
static u8 gpu64MirrorAwaitIrq = 1;
static u8 gpu64MirrorJiffy = 0;

u32 REU_SIZE_KB = 1024;

REUSTATE reu AAA;
u8 *reuMemory;
bool reuRunning;

static u64 armCycleCounter;

// gpu64: the hold-gap instrument (gpu64_holdgap.h). One cache line, core 0
// only. aligned(64) so it cannot share a line with anything else -- not
// because another core writes it, but because both warm paths preload it as
// a unit and a straddled struct would only be half-warmed.
GPU64HOLDGAP gpu64HoldGap __attribute__( ( aligned( 64 ) ) );

// gpu64: the hold gate's counters (gpu64_holdgate.h). Same shape and same
// reason as gpu64HoldGap above: one line of its own, warmed by both paths.
GPU64HOLDGATE gpu64HoldGate __attribute__( ( aligned( 64 ) ) );

// Called immediately after CLR_GPIO( bDMA_OUT ), i.e. with the bus already
// held, so this has no cycle budget to blow. stamp is armCycleCounter, which
// the caller's RESTART_CYCLE_COUNTER anchored at the start of the VIC
// half-cycle the assert happens in; both call sites are built the same way,
// so whatever bias that carries is identical on the release side and cancels.
__attribute__( ( always_inline ) ) inline void gpu64_holdGapAssert( u64 stamp, u8 kind )
{
	GPU64HOLDGAP *g = &gpu64HoldGap;
	g->asserts++;

	if ( g->lastKind == GPU64_HOLD_KIND_NONE )
		return;

	u64 d = stamp - g->lastRelease;
	u32 gap = ( d > 0xfffffffful ) ? 0xfffffffful : (u32)d;

	if ( gap < g->minGap )
		g->minGap = gap;

	u32 t = g->armPerC64;

	if ( g->lastKind == GPU64_HOLD_KIND_DISPATCH && kind == GPU64_HOLD_KIND_COMMIT )
	{
		if ( gap < g->minGapD2C )
			g->minGapD2C = gap;
		if ( t && gap < t * GPU64_HOLDGAP_CLOSE_CYCLES )
			g->d2cClose++;
	}

	// Bucket by whole C64 cycles. If armPerC64 never got measured everything
	// lands in the overflow bucket, which is a readable "no data" rather than
	// a divide by zero.
	if ( t == 0 )
	{
		g->bucket[ GPU64_HOLDGAP_BUCKETS - 1 ]++;
		return;
	}

	u32 b = 0;
	while ( b < GPU64_HOLDGAP_BUCKETS - 1 && gap >= t )
	{
		t <<= 1;
		b++;
	}
	g->bucket[ b ]++;
}

// Called immediately after SET_GPIO( bDMA_OUT ). Two stores to a line that
// both warm paths keep resident; see the cost note in gpu64_holdgap.h.
__attribute__( ( always_inline ) ) inline void gpu64_holdGapRelease( u64 stamp, u8 kind )
{
	gpu64HoldGap.lastRelease = stamp;
	gpu64HoldGap.lastKind = kind;
}

static volatile u8 forceRead;

#include "lowlevel_dma.h"

void resetREU()
{
	// gpu64: see the comment on gpu64ApiActive's declaration above -- this
	// is what actually clears it now.
	gpu64ApiActive = 0;

	// gpu64: and this is the mirror's only re-arm. A C64 reset is the one
	// event after which "BASIC owns the machine again" is true by
	// construction, so it is the one event that undoes the latch.
	gpu64MirrorEnabled = 1;
	gpu64MirrorSeenPrompt = 0;
	gpu64MirrorIrqWatchdog = 0;
	gpu64MirrorAwaitIrq = 1;
	gpu64MirrorJiffy = 0;
	// gpu64: the API register file resets with it -- a fresh REU session must
	// not inherit a previous program's staged ARGs, sticky CMD_HI or error.
	gpu64_apiReset();

	// gpu64: and so do the hold instruments, so a bench run's numbers are
	// that run's and not a previous program's.
	//
	// armPerC64 is the one field that survives. It is a property of the two
	// clocks, not of the run, and resetREU() is called from *inside* the
	// polling loop when the C64 derails -- long after the start-up block
	// that measures it, and with no way to measure it again. Zeroing it
	// there would silently disarm the hold gate's consecutiveness test
	// (gpu64_holdgate.h) for the rest of the session.
	register u32 armPerC64 = gpu64HoldGap.armPerC64;
	memset( &gpu64HoldGap, 0, sizeof( gpu64HoldGap ) );
	gpu64HoldGap.armPerC64 = armPerC64;
	gpu64HoldGap.minGap = 0xffffffff;
	gpu64HoldGap.minGapD2C = 0xffffffff;

	memset( &gpu64HoldGate, 0, sizeof( gpu64HoldGate ) );

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
// $D018 as read, not decoded: the decode is pure arithmetic and belongs in
// showMirror(), where there is no C64 waiting on the bus.
static u8 gpu64MirrorD018 = 0x15;

// gpu64: the mirror's clock is now the C64's own jiffy IRQ, counted at the
// $FFFF vector fetch in reuUsingPolling() below, not main-loop passes. The
// KERNAL programs CIA1 timer A for ~60Hz on both PAL and NTSC, so 15 ticks
// is ~4 snapshots a second. Measured in VICE with a non-stopping checkpoint
// on a load of $FFFF: exactly 178 hits in 178 jiffies at the BASIC prompt --
// one per jiffy, no strays -- and 0 hits on the NMI vector $FFFA/$FFFB, so
// RESTORE cannot clock the mirror. 15 ticks is the rate the old pass counter
// was calibrated to, and fast enough that a typed LOAD line and the cursor
// blink read normally on HDMI.
#define GPU64_MIRROR_JIFFY_INTERVAL 15

// gpu64: main-loop passes (~1 C64 cycle each) without a jiffy IRQ vector
// fetch before the mirror concludes that BASIC no longer owns the machine.
// ~1s at 985kHz, i.e. ~60 jiffies -- short enough that an ML program which
// takes the IRQ over stops the mirror well inside a second.
//
// It only starts counting once a vector fetch has actually been seen
// (gpu64MirrorAwaitIrq). That is not a refinement, it is the whole reason a
// reset used to leave HDMI frozen for good, reported from the bench
// 2026-09-06: "when I reset, the C64 resets but mirror stays freezed". The
// KERNAL's reset path runs RAMTAS *before* its CLI, and RAMTAS is a
// per-byte read/write/verify sweep of all ~38.9K of free RAM at ~50 cycles a
// byte -- close to two seconds with interrupts masked throughout. The
// re-arm on the reset line fired correctly; the watchdog then expired in the
// middle of the RAM test and cleared the latch again, and nothing but
// another reset could ever set it. The comment that used to be here put the
// RAM test at ~200ms, which is where the error came from.
#define GPU64_MIRROR_IRQ_TIMEOUT 1000000

// gpu64: the two C64 zero-page bytes the snapshot probes before it commits
// to the 2ms burst.
//
// CURLIN+1 ($3A) is BASIC's current line number, high byte: $FF while a
// direct-mode statement is being executed, the running line's number while a
// program executes. Measured in VICE (x64sc, binary monitor) on 2026-09-06:
// $FF at the prompt after any command, $00 for line 10 / $00 for line 20
// during RUN, $FF for the whole of a LOAD. So "$3A != $FF" is the canonical
// "a BASIC program is RUNning", and it is true from the first statement of
// RUN, before the program has drawn anything.
//
// The one exception, also measured, is the *cold* prompt: BASIC's cold start
// zeroes CURLIN, so $3A reads $00 from power-on until the first direct-mode
// statement runs, which is indistinguishable from "executing line 0-255".
// That is why the latch is armed by a prompt sighting first (see below) and
// why $3A takes no part in the bail decision -- gating the snapshot on
// $3A == $FF would mean never mirroring the fresh boot screen at all, which
// is the mirror's entire reason to exist.
//
// BLNSW ($CC) is the KERNAL's cursor-blink switch: 0 exactly while the
// input loop at the prompt is blinking the cursor, non-zero whenever the
// KERNAL is doing something else -- printing, LISTing, or driving the serial
// bus for LOAD/SAVE. It is not a program/no-program signal (it says nothing
// about ML), it is a "safe to steal 2ms of bus right now" signal: the IEC
// protocol is software-timed on both ends and a 2ms freeze mid-byte is
// exactly how a LOAD turns into ?LOAD ERROR. Measured in the same VICE
// session: $00 at the cold prompt and at the prompt between commands, $01
// for the whole of a RUN, $01 across SEARCHING FOR / the serial transfer of
// a LOAD (true drive emulation).
#define GPU64_C64_CURLIN_HI 0x003A
#define GPU64_C64_BLNSW     0x00CC

// gpu64: milestone 3 screen mirror. Grabs a brief DMA burst -- the same
// CLR_GPIO(bDMA_OUT)/DMA_READBYTE_P1..P3 cycle-stealing technique REU's own
// Store/Fetch transfers already use in handle_transfer.h, not a new kind of
// bus takeover -- to read the default screen RAM ($0400-$07E7) and color RAM
// ($D800-$DBE7), plus the current border/background color ($D020/$D021),
// then hands them to CRAD::showMirror() (rad_main.cpp) for rendering.
//
// WHEN this runs was reworked on 2026-09-06, for two independent reasons.
//
// (1) Correctness. This was the last async DMA hold in the tree: it fired on
// a free-running pass counter, so CLR_GPIO(bDMA_OUT) landed on whatever C64
// cycle the loop happened to be in. That is polling-loop rule 7 -- halting
// the 6510 through RDY stops it only on *read* cycles, and a write in flight
// completes with AEC already tri-stating the address bus, i.e. into a
// floating address. It is the same defect that killed the C64 through the
// whole Stage 16 campaign, and it was live here at 4 holds a second. It has
// never been implicated (BASIC's idle loop is nearly all reads, and the
// prompt touches IO2 never, so the sampled-IO2 gate the flip commit uses was
// not available to it) -- but "never observed" is not "cannot happen".
//
// The gate this now uses instead is the C64's own IRQ sequence: the loop
// sees the vector fetch at $FFFF (bTriggerFF00 is a hardware $FFxx decode,
// so unlike $A480 or $EA31 it is actually visible), and the very next 6510
// cycle after that fetch is the handler's opcode fetch -- a read, by
// construction, the same guarantee the IO2 gate rests on. It arrives once
// per jiffy, which is also exactly the clock the mirror wants.
//
// (2) Scope. The mirror exists so the user can see the READY prompt on HDMI
// and LOAD something without switching to the VIC-II screen. Once a program
// is actually running it has no business stealing 2ms of bus 4 times a
// second, so it now latches off for good, re-armed only by a C64 reset
// (resetREU()). Three things latch it off:
//
//   - CURLIN+1 != $FF, probed below: a BASIC program is RUNning.
//   - no jiffy IRQ for ~1s (GPU64_MIRROR_IRQ_TIMEOUT, checked in the loop):
//     something has taken the IRQ over or masked it -- an ML program.
//   - the gpu64 API being engaged at all (gpu64ApiActive), as before.
//
// Residual case, documented rather than fixed: an ML program SYSed from
// direct mode that leaves the KERNAL IRQ alone and never touches gpu64 keeps
// the latch armed. It costs almost nothing -- BLNSW will be non-zero, so
// every tick bails after the two probe bytes, a ~6 cycle hold at 4Hz -- and
// HDMI simply holds the last mirrored frame, which is the intended
// end state anyway.
// Scoped per project/bus_access_design.md: fixed default addresses only (no VIC
// bank detection yet -- a program that relocates its screen will render
// wrong until that's added; $D018's charset select *is* followed), standard
// text mode only, and gpu64's own bundled copy
// of the C64 character ROM (gpu64_c64font.h) rather than peeking the C64's
// real char ROM, which the CPU can't see when it's banked out anyway. That
// copy is const; the mirror used to draw from font_bin, which is neither the
// real ROM nor immutable -- see the note in CRAD::showMirror().
//
// __attribute__ set to match reuUsingPolling() below (same file, same
// reasoning: this file's cache-preloading/alignment choices are load-bearing
// for cycle-precise timing, see the comment above reuLoad32() usage in
// reuUsingPolling()'s tail). Needs its own instruction-cache preload before
// first use -- see warmCache() in rad_main.cpp -- since it's called from
// inside reuUsingPolling() but isn't covered by that function's own preload
// window.
__attribute__( ( optimize( "align-functions=256" ) ) )
__attribute__( ( section( ".text.section_polling" ) ) )
void gpu64_mirrorSnapshot()
{
	register u32 g2;
	register u16 c_a;
	register u8 x;

	// gpu64: WAIT_FOR_CPU_HALFCYCLE first -- see the long note in
	// gpu64_blobRead() below. The gate now calls us from inside the CPU
	// half-cycle, having just sampled the bus (it used to be the VIC half,
	// from the top of the loop); either way a bare WAIT_FOR_VIC_HALFCYCLE is
	// not a sync. It falls straight through when we are already in the VIC
	// half, RESTART_CYCLE_COUNTER then anchors mid-half-cycle,
	// TIMING_TRIGGER_DMA has already elapsed, and CLR_GPIO lands at an
	// undefined phase.
	WAIT_FOR_CPU_HALFCYCLE
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER
	WAIT_UP_TO_CYCLE( reu.TIMING_TRIGGER_DMA );
	CLR_GPIO( bDMA_OUT );

	// gpu64: probe first, burst second. Both bytes are read every tick --
	// CURLIN unconditionally, because BLNSW is non-zero for the whole of a
	// BASIC program's run and gating the CURLIN read on it would mean never
	// noticing RUN at all.
	c_a = GPU64_C64_CURLIN_HI;
	DMA_READBYTE_P1( c_a );
	DMA_READBYTE_P2();
	DMA_READBYTE_P3( x, false );
	register u8 curlinHi = x;

	c_a = GPU64_C64_BLNSW;
	DMA_READBYTE_P1( c_a );
	DMA_READBYTE_P2();
	DMA_READBYTE_P3( x, false );
	register u8 blnsw = x;

	// gpu64: the prompt sighting that arms the CURLIN latch. Direct mode plus
	// a blinking cursor is BASIC idle at READY and nothing else. It cannot
	// fire before the user's first command (see the $3A note above), which is
	// exactly the point -- $3A is only trustworthy once it has been seen to
	// hold $FF, and until then the latch stays disarmed rather than reading
	// the cold prompt's $00 as "line 0 is running".
	if ( curlinHi == 0xFF && blnsw == 0 )
		gpu64MirrorSeenPrompt = 1;
	else
	if ( curlinHi != 0xFF && gpu64MirrorSeenPrompt )
		// A BASIC program is running. Done mirroring until the next reset.
		gpu64MirrorEnabled = 0;

	// gpu64: BLNSW alone decides whether we may take the bus for 2ms; $3A
	// decides whether we keep mirroring at all, and has already had its say
	// above.
	register u8 bail = ( blnsw != 0 ) ? 1 : 0;

	if ( bail )
	{
		// gpu64: give the bus back on a cycle boundary, exactly like the
		// command dispatch does -- not wherever the probe finished. The two
		// DMA_READBYTE_P3s above already dropped the address latch and the
		// bus transceiver (DISABLE_ADDRESS_LATCH_AND_BUSTRANSCEIVER with
		// releaseDMA false), so the DMA line is all that is still ours.
		WAIT_FOR_CPU_HALFCYCLE
		WAIT_FOR_VIC_HALFCYCLE
		RESTART_CYCLE_COUNTER
		SET_GPIO( bDMA_OUT );
		return;
	}

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

	// gpu64: $D018, so the mirror follows PRINT CHR$(14)/CHR$(142) between
	// the ROM's two charsets. One more byte on a hold that has just read
	// 2000 of them; showMirror() does the decoding.
	c_a = 0xD018;
	DMA_READBYTE_P1( c_a );
	DMA_READBYTE_P2();
	DMA_READBYTE_P3( x, false );
	gpu64MirrorD018 = x;

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

	gpu64_showMirror( g_pRAD, gpu64MirrorScreen, gpu64MirrorColor, gpu64MirrorBorder, gpu64MirrorBackground, gpu64MirrorD018 );
}

// gpu64: the boot-phase mirror. RAD spends everything between "the C64 is
// running" and "the user pressed the button" in rad_main.cpp's radIsWaiting
// spin, which used to be a bare 1250-cycle button poll -- so HDMI showed
// nothing at all until the user had blind-navigated the RAD menu into REU
// mode. Reported from the bench 2026-09-06: "after start it does not
// activate, I have to press blindly REU menu button and then X to go to
// BASIC."
//
// This replaces that spin. It is the same $FFFF vector-fetch gate the REU
// polling loop runs (see the long comment at that gate, and rule 7 in
// CLAUDE.md): sample the bus every C64 cycle, and when the 6510 is fetching
// the high byte of the IRQ vector -- last cycle of the interrupt sequence,
// next cycle provably an opcode fetch -- take the bus and snapshot. Nothing
// else is being serviced here, so a missed sample costs one jiffy tick and
// nothing more; the phase discipline is what matters, not the throughput.
//
// Safe to run at this point in the boot flow because everything the sample
// sequence and the snapshot need is already set up: gpioInit() has
// configured the latch/OE/DIR/MPLEX/DMA pins, initREU() has filled in the
// reu.TIMING_* fields the WAIT_UP_TO_CYCLE calls read, and
// checkIfMachineRunning() has already blocked until the C64 clock is
// measurably alive -- so the half-cycle waits below cannot hang on a dead
// clock in the normal case. The abnormal case (clock stops while we sit
// here) is handled by bounding the top-of-loop wait rather than by hoping,
// because a hang here would also swallow the RAD button and leave the user
// with no way into the menu.
//
// Returns when the RAD button is pressed, i.e. exactly where the old spin's
// "goto hijacking" was.
__attribute__( ( optimize( "align-functions=256" ) ) )
__attribute__( ( section( ".text.section_polling" ) ) )
void gpu64_mirrorIdleLoop( void )
{
	register u32 g2 = bBUTTON, g3;
	register u16 resetCount = 0;
	register u32 ipl = 0;
	register u32 noClock = 0;

	// gpu64: warm both this loop and the snapshot it calls. warmCache() runs
	// on the way into REU emulation, which is *after* this -- nothing has
	// preloaded either one yet at this point in the boot.
	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)gpu64_mirrorIdleLoop, 1024 * 2 )
	FORCE_READ_LINEARa( (void*)gpu64_mirrorIdleLoop, 1024 * 2, 1024 * 2 );
	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)gpu64_mirrorSnapshot, 1024 * 4 )
	FORCE_READ_LINEARa( (void*)gpu64_mirrorSnapshot, 1024 * 4, 1024 * 4 );

	// gpu64: the mirror is armed on every entry here. resetREU() is the
	// other arm point and only runs once REU emulation starts; a user who
	// power-cycles into the menu and back out must not find the latch stuck
	// from a previous session.
	gpu64MirrorEnabled = 1;
	gpu64MirrorSeenPrompt = 0;
	gpu64MirrorIrqWatchdog = 0;
	gpu64MirrorAwaitIrq = 1;
	gpu64MirrorJiffy = 0;

mirrorIdleLoop:

	CLR_GPIO( bMPLEX_SEL );
	WAIT_FOR_CPU_HALFCYCLE
	BEGIN_CYCLE_COUNTER

	while ( 1 )
	{
		// gpu64: bounded form of WAIT_FOR_VIC_HALFCYCLE. The REU loop can use
		// the unbounded macro because by then the machine is committed; here
		// the RAD button still has to work, and a C64 whose clock stopped
		// while we were watching would otherwise strand the user in a spin
		// with no exit. On timeout we simply fall through with whatever g2
		// held: the button test below is all that still matters.
		noClock = 0;
		{
			register u32 timeout = 0;
			do {
				g2 = read32( ARM_GPIO_GPLEV0 );
				if ( ++timeout > 1000000 ) { noClock = 1; break; }
			} while ( CPU_HALF_CYCLE );
		}

		// gpu64: the button is tested before anything that needs the C64
		// clock, so a stopped clock degrades to exactly the button poll this
		// loop replaced rather than to a hang.
		if ( BUTTON_PRESSED )
			return;

		if ( noClock )
			continue;

		// gpu64: same incremental i-cache refresh the REU loop does. The
		// snapshot's showMirror() paints 40x25 cells into the framebuffer and
		// cleans a page, which is more than enough to evict this loop between
		// ticks -- and a cold miss in the sample sequence is a misread
		// address, which under the gate below is a DMA hold opened on a cycle
		// nothing is known about. Rule 4.
		void *p = (u8*)( && mirrorIdleLoop ) + ipl;
		CACHE_PRELOADIKEEP( p );
		ipl += 64; if ( ipl >= 1024 * 2 ) ipl = 0;

		// gpu64: re-arm on the C64's reset line -- pressing the RAD reset
		// button at the BASIC prompt is the user's way of saying "start
		// mirroring again", and before this it did nothing visible on HDMI at
		// all. resetREU() does the same thing for the REU-emulation half of
		// the machine's life; this is the boot-phase half, where resetREU()
		// never runs.
		//
		// On the RELEASE edge, not the assert: while the line is held the
		// 6510 is stopped and no jiffy IRQ arrives, so a press held longer
		// than GPU64_MIRROR_IRQ_TIMEOUT (~1s -- easy for a thumb) would let
		// the watchdog below retire the mirror again right after arming it.
		// Holding the watchdog clear for the duration and arming when the
		// machine actually restarts avoids having to reason about which of
		// the two wins.
		if ( CPU_RESET )
		{
			resetCount = 1;
			gpu64MirrorIrqWatchdog = 0;
		} else
		if ( resetCount )
		{
			resetCount = 0;
			gpu64MirrorEnabled = 1;
			gpu64MirrorSeenPrompt = 0;
			gpu64MirrorIrqWatchdog = 0;
			gpu64MirrorAwaitIrq = 1;
			gpu64MirrorJiffy = 0;
		}

		// gpu64: the liveness timer, same as the REU loop's. Without the
		// KERNAL IRQ there is no clock and no prompt to mirror.
		//
		// Suspended until the first vector fetch after a re-arm: the C64 we
		// just watched come out of reset spends ~2s in RAMTAS with interrupts
		// masked, which is twice the timeout. Waiting forever is the right
		// behaviour anyway -- a re-armed mirror with no IRQ takes no bus and
		// paints nothing, so there is nothing to time out of.
		if ( gpu64MirrorEnabled && !gpu64MirrorAwaitIrq )
		{
			if ( ++gpu64MirrorIrqWatchdog >= GPU64_MIRROR_IRQ_TIMEOUT )
				gpu64MirrorEnabled = 0;
		}

		// gpu64: the bus sample, copied cycle-for-cycle from
		// reuUsingPolling(). g2 carries RESET/BUTTON/RW/$FFxx, g3 the
		// multiplexed low address byte.
		SET_GPIO( bDIR_Dx );
		WAIT_FOR_CPU_HALFCYCLE
		RESTART_CYCLE_COUNTER
		WAIT_UP_TO_CYCLE( reu.WAIT_FOR_SIGNALS + reu.TIMING_OFFSET_CBTD );
		g2 = read32( ARM_GPIO_GPLEV0 );

		SET_GPIO( bMPLEX_SEL );

		WAIT_UP_TO_CYCLE( reu.WAIT_CYCLE_MULTIPLEXER );
		g3 = read32( ARM_GPIO_GPLEV0 );
		CLR_GPIO( bMPLEX_SEL );

		// gpu64: this fires the snapshot directly on the vector fetch,
		// i.e. on the original unconfirmed form of rule 7 -- it is the one
		// hold in the tree that gpu64_holdgate.h's forward confirmation was
		// deliberately NOT applied to. Two reasons, and both have to hold:
		//
		//  - The confirm's term 2 ("the previous pass really was the
		//    previous C64 cycle") is a comparison of PMCCNTR_EL0 stamps
		//    against gpu64HoldGap.armPerC64, and armPerC64 is measured on
		//    the way into REU emulation, which is *after* this loop. There
		//    is no calibration here to compare against, and a confirmation
		//    whose adjacency term always passes is not a confirmation.
		//
		//  - What the rule protects against is a program that reaches
		//    $FFFF, or an IO2 address, on a cycle that is not its
		//    instruction's last -- a read-modify-write, or an indexed
		//    access whose un-fixed address lands there. The only code alive
		//    at this point in the boot is the KERNAL and BASIC at the READY
		//    prompt, which reach $FFFF exactly once, as the interrupt
		//    sequence's vector fetch. Once a program that could do
		//    otherwise is running, this loop is no longer the one sampling
		//    the bus: reuUsingPolling() is, and it uses the confirmed gate.
		//
		// If this loop ever gains a caller that runs with user code live,
		// that second reason evaporates and this needs the real gate --
		// which means moving the armPerC64 calibration ahead of it first.
		if ( ADDRESS_FFxx && !CPU_WRITES_TO_BUS && ADDRESS0to7 == 0xFF &&
				gpu64MirrorEnabled )
		{
			gpu64MirrorIrqWatchdog = 0;
			gpu64MirrorAwaitIrq = 0;

			if ( ++gpu64MirrorJiffy >= GPU64_MIRROR_JIFFY_INTERVAL )
			{
				gpu64MirrorJiffy = 0;
				gpu64_mirrorSnapshot();
			}
		}
	}
}

// gpu64: commits a vblank-deferred PAGE_FLIP. Everything else a frame
// boundary does is inlined into the loop (gpu64_vsyncAdvance(),
// gpu64_vsync.h) precisely so this is the only call on the path -- and this
// one only happens when a flip is actually pending.
//
// WHEN it is called matters as much as what it does. The frame boundary only
// *arms* it (gpu64Vsync.commitDue); the loop fires it from the sampled-IO2
// gate just before noREUAccess:, where the C64's next cycle is provably a
// read. Taking the bus next to a C64 write cycle is what killed the C64 in
// bench runs M and N -- the long comment at that gate has the reasoning and
// the evidence.
//
// As of 2026-09-07 that gate no longer fires on the IO2 access itself: it
// arms there and opens one C64 cycle later, once that cycle has confirmed
// itself a read at a different address. gpu64_holdgate.h has the four terms
// and the two instruction shapes that made the old form unsound.
//
// The commit is bracketed in a DMA hold, exactly like the command dispatch
// and the mirror snapshot. SetVirtualOffset() is a mailbox round-trip to the
// VideoCore whose cost nobody has measured (it was open question 1 in
// project/milestone4_2d_api_design.md), and this runs while the C64 is
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
__attribute__( ( section( ".text.section_polling" ) ) )
void gpu64_vsyncCommitFlip( void )
{
	// The WAIT_FOR_*_HALFCYCLE macros sample the GPIO bank into a variable
	// they expect to be called g2, same as gpu64_mirrorSnapshot() above.
	register u32 g2;
	(void)g2;

	// gpu64: WAIT_FOR_CPU_HALFCYCLE first, same reason as
	// gpu64_mirrorSnapshot() above -- WAIT_FOR_VIC_HALFCYCLE alone is not a
	// sync and the bus would be taken at whatever phase the caller finished
	// on. The gate now calls us from inside the CPU half-cycle (it has just
	// sampled the bus), possibly late enough to have slipped past the
	// falling edge, which is exactly the case the dispatch hold's own
	// WAIT_FOR_CPU_HALFCYCLE covers. This fires once a frame for the whole
	// run, which is exactly the shape of damage that accumulates.
	WAIT_FOR_CPU_HALFCYCLE
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER
	WAIT_UP_TO_CYCLE( reu.TIMING_TRIGGER_DMA );
	CLR_GPIO( bDMA_OUT );
	gpu64_holdGapAssert( armCycleCounter, GPU64_HOLD_KIND_COMMIT );

	gpu64_commitFlip();

	// gpu64 (2026-09-08): the whole STATUS update happens INSIDE the hold,
	// and the FRAME_READY harvest happens here rather than only at the next
	// dispatch. Two things follow from the placement, both deliberate:
	//
	// 1. gpu64_3dPollLoopFrame() reads gpu64_3dRing.tail, which core 1
	//    writes. CLAUDE.md forbids that read anywhere reuUsingPolling() can
	//    reach with the bus free-running -- but the bus is held here, so the
	//    per-C64-cycle deadline that rule is about does not exist, exactly
	//    as it does not for a command dispatch. Its i-cache is warmed by
	//    gpu64_vsyncWarmCommit() alongside this function's own.
	// 2. The three writes are now atomic from the C64's point of view: it
	//    cannot sample STATUS at all between them. Clearing BUSY after the
	//    release left a window in which a program polling the documented
	//    "FRAME_READY set and BUSY clear" predicate could see the frame
	//    ready but the flip apparently still pending, and go round again.
	//
	// Cost is one non-blocking modular-distance test and three stores added
	// to a hold that already contains a VideoCore mailbox round trip.
	gpu64_3dPollLoopFrame();
	gpu64Vsync.flipPending = 0;
	gpu64Regs.status &= ~GPU64_STATUS_BUSY;

	WAIT_FOR_CPU_HALFCYCLE
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER
	SET_GPIO( bDMA_OUT );
	gpu64_holdGapRelease( armCycleCounter, GPU64_HOLD_KIND_COMMIT );
}

// gpu64: called from the PAGE_FLIP dispatch, with the bus held, to warm the
// commit path before the loop needs it. The dispatch that queues a flip is
// the biggest instruction-cache consumer in the system and will have evicted
// whatever warmCache() preloaded at REU start -- see the general rule in
// project/progress_tracker.md. Doing it here rather than at the point of use is
// what keeps the loop's own exposure to a single short call.
void gpu64_vsyncWarmCommit( void )
{
	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)gpu64_vsyncCommitFlip, 1024 * 2 );
	FORCE_READ_LINEARa( (void*)gpu64_vsyncCommitFlip, 1024 * 2, 1024 * 2 );

	// gpu64: and the mailbox post the commit now ends in, which lives in
	// another translation unit and is not covered by the preload above.
	gpu64_flipWarm();

	// gpu64 (2026-09-08): and the FRAME_READY harvest the commit now ends
	// in, which is in gpu64_3d_class1.cpp and so is not covered by the
	// preload above either. Warming it here, from a dispatch that already
	// holds the bus, is rule 5 -- warming it at the point of use would put
	// the delay inside the once-a-frame hold it is meant to shorten.
	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)gpu64_3dPollLoopFrame, 512 );
	FORCE_READ_LINEARa( (void*)gpu64_3dPollLoopFrame, 512, 512 );

	// gpu64: and the hold-gap instrument's single line, so the store the
	// commit's release does cannot take a cold miss in the one window where
	// the C64 is already free-running again (gpu64_holdgap.h, rule 4).
	CACHE_PRELOAD_DATA_CACHE( &gpu64HoldGap, sizeof( gpu64HoldGap ), CACHE_PRELOADL1KEEP )
	FORCE_READ_LINEAR32a( &gpu64HoldGap, sizeof( gpu64HoldGap ), sizeof( gpu64HoldGap ) * 8 );
	CACHE_PRELOAD_DATA_CACHE( &gpu64HoldGate, sizeof( gpu64HoldGate ), CACHE_PRELOADL1KEEP )
	FORCE_READ_LINEAR32a( &gpu64HoldGate, sizeof( gpu64HoldGate ), sizeof( gpu64HoldGate ) * 8 );
}

// gpu64: forces every cache line of a buffer resident before a DMA burst
// walks it, and does it with real loads rather than a prfm hint -- prfm is
// advisory and the hardware is free to drop it, and this is the one place
// where "mostly warmed" and "warmed" differ by a corrupted byte.
//
// This replaced a 1024-byte warm that was the cause of the last failing
// conformance check. An 8000-byte upload came back with a handful of wrong
// bytes, always beyond offset 1024 -- eleven of them across five hardware
// runs, the lowest at 1057 -- and a second, warm upload of the same data was
// clean every time. Past the warm window every 64th store lands on a cold
// line, the core has to read-allocate it from an SDRAM the VideoCore is also
// using, and the stall outlasts the C64 cycle the burst is riding. The byte
// sampled is then whatever the bus moved on to.
//
// Free in bus terms: the C64 is DMA-halted here, and the warm costs one load
// per cache line against a burst that spends a whole C64 cycle per byte.
static inline void gpu64_warmBuffer( const u8 *p, u32 len )
{
	__attribute__( ( unused ) ) volatile u8 forceRead;
	for ( u32 i = 0; i < len; i += 64 )
		forceRead = p[ i ];
	if ( len )
		forceRead = p[ len - 1 ];
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
__attribute__( ( section( ".text.section_polling" ) ) )
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
	// ...and the destination, ALL of it. This used to warm the first 1024
	// bytes only, which is what made every large transfer come back with a
	// few wrong bytes -- see gpu64_warmBuffer() above for the evidence.
	gpu64_warmBuffer( pDst, len );

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
__attribute__( ( section( ".text.section_polling" ) ) )
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
	gpu64_warmBuffer( pSrc, len );			// all of it -- see gpu64_blobRead()

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

// gpu64: reuUsingPolling()'s own entry address, captured on the way in. Only
// used by gpu64_apiWarmPollingLoop() below.
static void *s_PollLoopBase = 0;

// gpu64: re-warms everything the polling loop depends on and a command
// dispatch has just evicted. Called at the end of a class 1 dispatch, with
// the bus still held -- rule 5 in project/progress_tracker.md's polling-loop
// timing rules: warm from upstream, where the C64 is already stopped, never
// at the point of use.
//
// Why this is needed at all is rule 4: "preloaded at start-up" is not
// durable. A single class 0 CLEAR walking 64000 bytes was enough to evict
// warmCache()'s work; a DRAW_MESH walks a framebuffer, a z-buffer and an
// arena, which is far more. Two distinct casualties:
//
//   1. The loop's instruction window. It partly self-heals -- the loop rolls
//      a CACHE_PRELOADIKEEP 64 bytes forward every pass -- but that is ~106
//      passes to come fully back, and the loop runs cold for all of them.
//   2. gpu64Regs and gpu64Vsync. rad_main.cpp's warmCache() preloads these
//      L1KEEP precisely because the loop touches them on every ARG write and
//      every STATUS read, and *nothing* re-warms them after a dispatch. This
//      is the same hole gpu64_vsyncWarmCommit() already plugs for the flip
//      commit path.
void gpu64_apiWarmPollingLoop( void )
{
	// gpu64: re-warm the deferred-flip commit path if one is armed, and do it
	// *before* the loop preload below so the loop is always the last thing to
	// touch the instruction cache before the bus is released.
	//
	// gpu64_vsyncWarmCommit() is otherwise issued exactly once, by the
	// PAGE_FLIP / SCENE_COMMIT that arms the flip. Until Stage 16 that was
	// enough: the only program that ever armed a deferred flip
	// (gpu64_vblank_demo.a) then sat in a STATUS read loop until BUSY
	// cleared, so nothing ran between the arm and the boundary. Stage 16's
	// SCENE_COMMIT returns immediately and the C64 keeps issuing commands, so
	// several full dispatches now run inside the armed window, each one the
	// biggest instruction-cache consumer in the system -- and the loop preload
	// below cannot cover the commit path, which sits 0xE00 bytes *before*
	// s_PollLoopBase (0x85a00 vs 0x86800 as linked), outside the window.
	//
	// So the commit was firing cold once a frame, at the one place where a
	// stall between RESTART_CYCLE_COUNTER and the GPIO write takes or gives
	// back the bus at an undefined phase. Warming here is rule 5: every caller
	// still holds the bus, so the preload costs hold time rather than a missed
	// C64 access, and it only runs while a flip is actually armed.
	//
	// BENCH A/B, 2026-09-05: still a hypothesis. The three runs that first
	// carried it showed gpu64_loop_test's SEQ/SEQACK drop detector at a
	// remarkably constant ~0.19 lost C64 writes per accepted flip (4/26,
	// 25/129, 10/51) where the pre-fix run reported zero. Set this to 0 to
	// build the other arm and find out whether this warm is the author of
	// those drops or merely the first build to run alongside them.
#define GPU64_WARM_COMMIT_AT_DISPATCH 1

#if GPU64_WARM_COMMIT_AT_DISPATCH
	if ( gpu64Vsync.flipPending )
		gpu64_vsyncWarmCommit();
#endif

	if ( s_PollLoopBase )
		CACHE_PRELOAD_INSTRUCTION_CACHE( s_PollLoopBase, GPU64_POLL_IPL_WINDOW );

	CACHE_PRELOAD_DATA_CACHE( &gpu64Regs, sizeof( GPU64REGS ), CACHE_PRELOADL1KEEP )
	FORCE_READ_LINEAR32a( &gpu64Regs, sizeof( GPU64REGS ), sizeof( GPU64REGS ) * 8 );

	CACHE_PRELOAD_DATA_CACHE( &gpu64Vsync, sizeof( GPU64VSYNC ), CACHE_PRELOADL1KEEP )
	FORCE_READ_LINEAR32a( &gpu64Vsync, sizeof( GPU64VSYNC ), sizeof( GPU64VSYNC ) * 8 );
	CACHE_PRELOAD_DATA_CACHE( &gpu64HoldGap, sizeof( gpu64HoldGap ), CACHE_PRELOADL1KEEP )
	FORCE_READ_LINEAR32a( &gpu64HoldGap, sizeof( gpu64HoldGap ), sizeof( gpu64HoldGap ) * 8 );
	CACHE_PRELOAD_DATA_CACHE( &gpu64HoldGate, sizeof( gpu64HoldGate ), CACHE_PRELOADL1KEEP )
	FORCE_READ_LINEAR32a( &gpu64HoldGate, sizeof( gpu64HoldGate ), sizeof( gpu64HoldGate ) * 8 );
}

// gpu64: this and the other four functions sharing the ".text.section_polling"
// section (gpu64_mirrorSnapshot, gpu64_vsyncCommitFlip, gpu64_blobRead,
// gpu64_blobWrite, below) are grouped together deliberately -- see each
// site's own comment for why. The ".text." prefix is load-bearing, not
// cosmetic: circle.ld (external/, gitignored, not ours to edit) places
// *(.text*) before it sets _etext, and translationtable64.cpp's MMU setup
// marks every 64KB page whose base is >= _etext execute-never (PXN). A bare
// section name -- what this used to be -- is an orphan section ld places
// *after* .text, i.e. entirely past _etext, so every function in it lived on
// borrowed time: fine as long as the whole group fit in the last 64KB page
// shared with the rest of .text, a silent crash the moment growth elsewhere
// in the image pushed reuUsingPolling's tail across the next page boundary.
// That is exactly what stage 14 did: two harmless-looking Stage-14 builds
// (12 opcodes, no DRAW_NODE) fit; the two that also linked DRAW_NODE did
// not, and the ~2KB tail of this function that landed past the boundary
// faulted on fetch -- IO2 unserviced, floating bus reads, dead RAD menu,
// power-cycle required. Diagnosed 2026-08-29 by hardware bisection down to
// DRAW_NODE, then confirmed by reading _etext and section_polling's
// placement out of the ELF with nm/readelf, not by guessing. The ".text."
// prefix makes this function part of .text by construction, so it is always
// on the PXN=0 side of _etext regardless of image size.
#if 1
__attribute__( ( optimize( "align-functions=256" ) ) )
__attribute__( ( section( ".text.section_polling" ) ) )
u8 reuUsingPolling( int step )
{
	register u32 g2 = bBUTTON, g3;
	register u16 resetCount = 0;

	// gpu64: milestone 6's load-ladder instrumentation (gpu64_ladder.h).
	// Expands to nothing unless GPU64_LADDER_ENABLED is defined.
	GPU64_LADDER_LOCALS

	u16 ipl = 0;

	// gpu64: the hold gate (gpu64_holdgate.h). All of it lives in registers
	// for the life of the loop -- the per-pass cost is the two moves at the
	// bottom of the body and one predicted-not-taken branch at the gate.
	register u32 gateArmed = GPU64_GATE_NONE;
	register u32 gateD = 0;				// CMD_LO payload of a deferred dispatch
	register u32 gateAge = 0;			// C64 cycles this arm has been waiting
	register u32 prevAnchor = 0;		// what the *previous* pass sampled, as
										//   GPU64_ANCHOR_* | low address byte
	register u64 prevStamp = 0;			// armCycleCounter of the previous pass
	register u32 gateSpan = 0;			// ARM cycles still counting as "next cycle"

	// gpu64: the loop's own base address, published so a dispatch can re-warm
	// the window it just evicted (gpu64_apiWarmPollingLoop() below). The label
	// is local to this function, so there is no other way to reach it from
	// another translation unit.
	s_PollLoopBase = (void *)( && reuEmulationMainLoop );

	if ( step <= 1 )
	{
		void *p = ( && reuEmulationMainLoop );
		CACHE_PRELOAD_INSTRUCTION_CACHE( p, GPU64_POLL_IPL_WINDOW );

		if ( step == 1 ) return 0;

		// gpu64: this block is also where armPerC64 comes from
		// (gpu64_holdgap.h). Each pass through the pair below is exactly one
		// C64 cycle, and RESTART_CYCLE_COUNTER already stamps every one of
		// them, so the conversion factor costs two compares per iteration
		// against a ~600-ARM-cycle half-cycle -- and the bus is held for all
		// of it. The window starts at 4000 rather than 0 so the measurement
		// is taken with this loop's own i-cache lines already resident.
		u64 calib0 = 0, calib1 = 0;

		for ( u16 i = 0; i < 20000; i++ )
		{
			WAIT_FOR_CPU_HALFCYCLE
			WAIT_FOR_VIC_HALFCYCLE
			RESTART_CYCLE_COUNTER
			if ( i == 4000 ) calib0 = armCycleCounter;
			if ( i == 19999 ) calib1 = armCycleCounter;
		}

		gpu64HoldGap.armPerC64 = (u32)( ( calib1 - calib0 ) / ( 19999 - 4000 ) );

		SET_GPIO( bDMA_OUT );
		gpu64_holdGapRelease( armCycleCounter, GPU64_HOLD_KIND_STARTUP );
	}

	// gpu64: "the pass I have just taken sampled the C64 cycle immediately
	// after the one I armed on" is the load-bearing claim of the hold gate,
	// and this is what proves it. RESTART_CYCLE_COUNTER stamps every pass at
	// the same point in the cycle, so two consecutive passes are one
	// armPerC64 apart and a skipped cycle is two; half a C64 cycle of slack
	// covers the jitter of arriving late into the CPU half without coming
	// anywhere near the skipped-cycle case.
	//
	// armPerC64 is 0 only on the VSF-injection entry, reuUsingPolling( 2 ),
	// which skips the calibration block above entirely. There the span is
	// made infinite, so the gate keeps the pre-2026-09-07 behaviour -- fire
	// on the next pass whatever its age. That is no weaker than what shipped,
	// because what shipped tested nothing at all; it is simply the one path
	// this fix cannot strengthen.
	gateSpan = gpu64HoldGap.armPerC64
			? gpu64HoldGap.armPerC64 + ( gpu64HoldGap.armPerC64 >> 1 )
			: 0xffffffff;

reuEmulationMainLoop:

	CLR_GPIO( bMPLEX_SEL );
	WAIT_FOR_CPU_HALFCYCLE
	BEGIN_CYCLE_COUNTER

	while ( 1 )
	{
		WAIT_FOR_VIC_HALFCYCLE

		GPU64_LADDER_SAMPLE

		void *p = (u8*)( && reuEmulationMainLoop ) + ipl;
		CACHE_PRELOADIKEEP( p );
		ipl += 64; if ( ipl >= GPU64_POLL_IPL_WINDOW ) ipl = 0;

		if ( CPU_RESET )
		{
			resetCount ++;
			if ( resetCount > 1000 )
			{
				// gpu64: the C64 has derailed and taken the REU with it.
				// That is the run whose ladder numbers matter most, and
				// until now it was also the run that lost them -- the test
				// PRG never gets to its closing LOG_ENABLE(1). Dump on the
				// way out. Costs a full log repaint, which is free here:
				// nothing is being timed any more.
				GPU64_LADDER_DUMP
				resetREU();
			}
		} else
			resetCount = 0;

		if ( BUTTON_PRESSED )
		{
			GPU64_LADDER_DUMP
			return 2;
		}

		// gpu64: default-state screen mirror (milestone 3), watchdog half.
		// The snapshot itself is NOT taken here any more -- this point is
		// ahead of the loop's bus sample, so nothing is known about the cycle
		// a DMA assert would land on, and that was rule 7 live at 4Hz. It now
		// fires from the $FFFF vector-fetch gate further down, where the
		// C64's next cycle is provably a read. See gpu64_mirrorSnapshot().
		//
		// All that is left here is the liveness timer: if the jiffy IRQ stops
		// arriving, BASIC is no longer driving the machine and the mirror
		// retires until the next reset.
		// Suspended until the first vector fetch after a re-arm -- see
		// GPU64_MIRROR_IRQ_TIMEOUT: the reset that re-armed the latch is
		// followed by ~2s of RAMTAS with interrupts masked, and timing out
		// inside that window is what froze HDMI after every reset.
		if ( !gpu64ApiActive && gpu64MirrorEnabled && !gpu64MirrorAwaitIrq )
		{
			if ( ++gpu64MirrorIrqWatchdog >= GPU64_MIRROR_IRQ_TIMEOUT )
				gpu64MirrorEnabled = 0;
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
				// gpu64: do not commit here. This point is *ahead* of the
				// loop's bus sample, so nothing is yet known about the cycle
				// the DMA assert would land on -- and a commit that lands
				// beside a C64 write cycle corrupts the C64 (bench runs
				// M/N, 2026-09-06; see gpu64_vsyncCommitFlip()). Record that
				// the flip is owed and let the sampled-IO2 gate below fire
				// it. Two flags rather than one because flipPending is the
				// API's "a flip is in flight" state, visible to the C64 as
				// STATUS.BUSY, while commitDue is purely this loop's.
				if ( gpu64Vsync.flipPending && !gpu64Vsync.commitDue )
				{
					gpu64Vsync.commitDue = 1;
					gpu64Vsync.commitDueFrame = gpu64Vsync.frameCount;
				}
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
				GPU64_LADDER_SKIP
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
				// section of project/project_description.md); a command that
				// moves a payload steals the bus for one bounded burst,
				// proportional to a length the C64 itself chose.
				if ( addr >= GPU64_REG_CMD_HI )
				{
					if ( addr == GPU64_REG_CMD_LO )
					{
						// gpu64: is this write provably the last cycle of its
						// instruction? (rule 7, gpu64_holdgate.h)
						//
						// Looking backwards is enough here, and it is the only
						// direction available -- the hold has to open on the
						// very next cycle or the C64 runs on. A write to CMD_LO
						// that is *not* an instruction's last cycle is a
						// read-modify-write's first write, and an RMW always
						// reads the same address on the cycle immediately
						// before it (inc $DF0C: c4 reads, c5 and c6 write;
						// inc $DF0C,X: c4 dummy-reads, c5 reads, c6 and c7
						// write). So: previous *C64 cycle*, an IO2 read, same
						// low address byte -- and the write is ambiguous.
						//
						// Everything else dispatches exactly as it has since
						// milestone 4, same instruction sequence, same timing.
						// That is deliberate: this is the one hold in the tree
						// that a thousand hardware runs have already hardened,
						// and it is worth more than the tidiness of routing it
						// through the gate below.
						//
						// The ambiguous case arms the gate instead. An RMW's
						// *second* write then dispatches immediately here --
						// its previous cycle is a write, not a read -- which
						// discards the deferred arm and runs the command once,
						// with the incremented value, on the instruction's
						// genuine last cycle. `inc $DF0C` becomes correct
						// rather than merely safe.
						if ( prevAnchor == ( GPU64_ANCHOR_READ | addr ) &&
								(u32)( armCycleCounter - prevStamp ) <= gateSpan )
						{
							gateArmed = GPU64_GATE_DISPATCH;
							gateD = D;
							gateAge = 0;
							gpu64HoldGate.armed++;
							gpu64HoldGate.dispatchDeferred++;
							GPU64_LADDER_SKIP
						} else
						{
						// A pending arm cannot survive an immediate dispatch:
						// it would fire again later, with a stale payload.
						gateArmed = GPU64_GATE_NONE;

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
						// gpu64: WAIT_FOR_CPU_HALFCYCLE first, so this catches
						// the *transition* -- see gpu64_blobRead(). We normally
						// arrive here inside the CPU half-cycle (the loop just
						// sampled the write), where this costs nothing, but the
						// register decode above has no cycle budget and a slip
						// past the falling edge would otherwise leave the bare
						// WAIT_FOR_VIC_HALFCYCLE falling through.
						WAIT_FOR_CPU_HALFCYCLE
						WAIT_FOR_VIC_HALFCYCLE
						RESTART_CYCLE_COUNTER
						WAIT_UP_TO_CYCLE( reu.TIMING_TRIGGER_DMA );
						CLR_GPIO( bDMA_OUT );
						gpu64_holdGapAssert( armCycleCounter, GPU64_HOLD_KIND_DISPATCH );

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
						gpu64_holdGapRelease( armCycleCounter, GPU64_HOLD_KIND_DISPATCH );
						GPU64_LADDER_SKIP
						}
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

		// gpu64: the deferred flip commit fires here, and only here.
		//
		// Reaching this point with IO2_ACCESS set means the loop has just
		// serviced a C64 load or store to $DFxx -- and an absolute-addressed
		// load or store spends its *last* cycle on that access. So the C64's
		// next cycle is an opcode fetch (or, if an interrupt is pending, the
		// interrupt sequence's two dummy reads, which come before its three
		// pushes): a read, either way. Asserting DMA_OUT now therefore halts
		// the CPU via RDY on a read cycle, with no write in flight.
		//
		// That guarantee is the whole point. The old code committed from the
		// frame-clock branch at the top of the pass, where the cycle about to
		// happen is unknown, and the C64's own multiplexed bus makes it
		// unknowable: the CPU only drives R/W during the PHI2-high half, by
		// which time it is too late to halt that cycle. Runs M and N put a
		// number on the cost -- a test that only *reads* a register inside
		// the armed window survived 300 flips, while one that *writes*
		// registers (without dispatching) died after 114 and 72. Same
		// dispatch, same hold, same everything else; writes were the only
		// difference.
		//
		// Consequence for API users: a deferred PAGE_FLIP now lands on the
		// program's next $DFxx access rather than exactly at the boundary.
		// Every handshake-mode program polls STATUS while BUSY is set, so in
		// practice that is the very next instruction. commitLateMax records
		// the worst case seen, for GET_HEALTH/logging to report.
		//
		// That was the reasoning as shipped, and it had a hole: the "last
		// cycle" claim is true of absolute loads and stores, which is all the
		// API itself uses, and false of a read-modify-write (inc $DF0D reads
		// on c4 and writes on c5 and c6) and of an indexed store, whose
		// unconditional dummy read can land in IO2 one cycle before the
		// write. Recorded as an API constraint at the time; closed
		// 2026-09-07 by not firing here at all.
		//
		// What happens instead is that this only *arms* the gate, and the
		// confirm at noREUAccess: below opens the hold one C64 cycle later,
		// after that cycle has proved itself a read at a different address.
		// The reasoning is in gpu64_holdgate.h. Everything above still holds
		// -- it is why an IO2 access is the right thing to arm on -- it is
		// just no longer the whole proof.
		//
		// gateArmed is tested so a deferred dispatch, armed a few lines
		// above, is not overwritten by a commit; the commit simply arms on
		// the next IO2 access instead.
		if ( gpu64Vsync.commitDue && IO2_ACCESS && !gateArmed )
		{
			gateArmed = GPU64_GATE_COMMIT;
			gateAge = 0;
			gpu64HoldGate.armed++;
		}

		// gpu64: and the screen mirror fires here, for the same reason and on
		// the same kind of guarantee.
		//
		// The C64 has no reason to touch IO2 at a BASIC prompt, so the mirror
		// cannot use the gate above. What it can use is the interrupt
		// sequence itself. Reaching this point with ADDRESS_FFxx set on a
		// read of $FF means the 6510 is fetching the high byte of the
		// IRQ/BRK vector -- the last cycle of the interrupt sequence, whose
		// two dummy reads and three pushes are already behind it. The next
		// cycle is the handler's opcode fetch. A read, by construction, so
		// DMA_OUT halts the CPU there with no write in flight.
		//
		// It is also the jiffy clock: this fires ~60 times a second while the
		// KERNAL IRQ is alive, and stops the moment it isn't -- which is the
		// signal the watchdog at the top of the loop is timing.
		//
		// The NMI vector is $FFFA/$FFFB, so RESTORE cannot reach this. The
		// write case is excluded because a write to $FFxx says nothing about
		// the next cycle (and $FF00 is REU's own transfer trigger, handled
		// far above). This gate carried the same hole as the commit gate --
		// an RMW whose read cycle landed on $FFFF would be followed by a
		// write -- and it is closed the same way: arm here, confirm below.
		// The watchdog and the jiffy counter are not part of the hold and
		// still update on the access itself.
		if ( ADDRESS_FFxx && !CPU_WRITES_TO_BUS && ADDRESS0to7 == 0xFF &&
				!gpu64ApiActive && gpu64MirrorEnabled )
		{
			gpu64MirrorIrqWatchdog = 0;
			gpu64MirrorAwaitIrq = 0;

			if ( ++gpu64MirrorJiffy >= GPU64_MIRROR_JIFFY_INTERVAL && !gateArmed )
			{
				gpu64MirrorJiffy = 0;
				gateArmed = GPU64_GATE_MIRROR;
				gateAge = 0;
				gpu64HoldGate.armed++;
			}
		}

	noREUAccess:
		// gpu64: the hold gate's confirm. Every hold this loop opens that is
		// not the CMD_LO dispatch opens here, and nowhere else.
		//
		// The proof is about two cycles, and neither of them is the one that
		// armed the gate. Let N be the cycle the *previous* pass sampled and
		// N+1 the one this pass just sampled. The hold may open during N+1
		// iff all four of these hold:
		//
		//   1. N was an IO2 access, or the $FFFF vector fetch -- prevAnchor.
		//   2. the previous pass really was the previous C64 cycle: the two
		//      RESTART_CYCLE_COUNTER stamps are under 1.5 armPerC64 apart. A
		//      skipped cycle is 2.0 apart and fails, and without this test
		//      term 4 would be comparing against an unrelated cycle.
		//   3. N+1 is a read. A write here says N was *not* an instruction's
		//      last cycle -- it was a read-modify-write's read, or its first
		//      write -- and asserting DMA under a write in flight tri-states
		//      the address bus beneath it. That is the Stage 16 derail.
		//   4. N+1 is a read at a *different* low address byte than N. This
		//      is what separates inc $DFxx,X's dummy read (c4) from its real
		//      read (c5): indexed addressing only ever fixes up the high
		//      byte, so those two share a low byte and c5 must not be allowed
		//      to confirm c4. The same test covers the page-crossing forms of
		//      lda $DFxx,X and lda ($xx),Y, and subsumes "not the same
		//      register twice".
		//
		// Together those make N+1 an opcode fetch, hence N+2 a read, because
		// no 6502 instruction and no interrupt sequence writes before its
		// third cycle. Full argument in gpu64_holdgate.h.
		//
		// Note what is *not* in the list: how long ago the arm happened. The
		// four terms are self-contained, so an arm that cannot fire simply
		// waits -- through the rest of an inc $DF0D, or through a whole REU
		// transfer -- and fires on the first pair of cycles that qualifies.
		// gateAge counts how long that took, in C64 cycles.
		if ( gateArmed )
		{
			if ( prevAnchor &&
					(u32)( armCycleCounter - prevStamp ) <= gateSpan &&
					CPU_READS_FROM_BUS &&
					ADDRESS0to7 != ( prevAnchor & 0xff ) )
			{
				register u32 kind = gateArmed;
				gateArmed = GPU64_GATE_NONE;
				gpu64HoldGate.fired++;
				if ( gateAge > gpu64HoldGate.ageMax )
					gpu64HoldGate.ageMax = gateAge;

				if ( kind == GPU64_GATE_COMMIT )
				{
					register u32 late = gpu64Vsync.frameCount - gpu64Vsync.commitDueFrame;
					if ( late > gpu64Vsync.commitLateMax )
						gpu64Vsync.commitLateMax = late;

					gpu64Vsync.commitDue = 0;
					gpu64_vsyncCommitFlip();
				} else
				if ( kind == GPU64_GATE_MIRROR )
				{
					gpu64_mirrorSnapshot();
				} else
				{
					// The deferred dispatch. Same sequence as the inline one
					// above, deliberately duplicated rather than shared: a
					// call would have to be made with the bus still free and
					// could take a cold i-cache miss there (rule 5), and this
					// branch is inside the window the loop preloads.
					gpu64HoldGate.dispatchFired++;

					WAIT_FOR_CPU_HALFCYCLE
					WAIT_FOR_VIC_HALFCYCLE
					RESTART_CYCLE_COUNTER
					WAIT_UP_TO_CYCLE( reu.TIMING_TRIGGER_DMA );
					CLR_GPIO( bDMA_OUT );
					gpu64_holdGapAssert( armCycleCounter, GPU64_HOLD_KIND_DISPATCH );

					gpu64_apiDispatch( gateD );

					WAIT_FOR_CPU_HALFCYCLE
					WAIT_FOR_VIC_HALFCYCLE
					RESTART_CYCLE_COUNTER
					SET_GPIO( bDMA_OUT );
					gpu64_holdGapRelease( armCycleCounter, GPU64_HOLD_KIND_DISPATCH );
				}

				GPU64_LADDER_SKIP
			} else
			{
				gateAge++;
				gpu64HoldGate.declined++;
			}
		}

		// gpu64: the gate's whole per-pass cost, and it has to run on every
		// path -- the non-IO2 fast path above jumps straight to this label,
		// which is exactly why the gate lives here and not beside the arms.
		//
		// prevAnchor records what this pass sampled, for the next pass to
		// reason about: nothing, an IO2 write, an IO2 read, or the $FFFF
		// vector fetch, packed with the low address byte so both of the
		// address comparisons above are a single compare. prevStamp is this
		// pass's RESTART_CYCLE_COUNTER anchor -- and when the pass held the
		// bus, it is the *release* anchor, which is the right one: the C64
		// was stopped in between, so the next pass really does sample the
		// next C64 cycle.
		prevAnchor = IO2_ACCESS
				? ( CPU_READS_FROM_BUS ? GPU64_ANCHOR_READ : GPU64_ANCHOR_WRITE ) | ADDRESS0to7
				: ( ADDRESS_FFxx && CPU_READS_FROM_BUS && ADDRESS0to7 == 0xFF )
					? ( GPU64_ANCHOR_READ | 0xFF )
					: 0;
		prevStamp = armCycleCounter;

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