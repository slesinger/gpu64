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

// gpu64: forward-declared rather than pulling in rad_main.h (which drags in
// the whole Circle screen/HDMI-console stack) -- see rad_main.h for the
// actual definition/assignment. Used below to reach CRAD::showTestPattern()
// when the C64 writes the gpu64 trigger register at IO2 $DF0B.
class CRAD;
extern CRAD *g_pRAD;
void gpu64_showTestPattern( CRAD *pRAD );
void gpu64_showMirror( CRAD *pRAD, const u8 *screen, const u8 *color, u8 border, u8 background );

// gpu64: set once a C64 program engages the gpu64 API -- currently that's
// just milestone 2's test-pattern trigger at $DF0B, see the IO2_ACCESS
// switch below. Per docs/bus_access_design.md, screen-mirror mode and
// framebuffer-API mode are mutually exclusive: once true, the periodic
// mirror snapshot in reuUsingPolling()'s main loop stops firing. Nothing
// currently clears it back to false -- milestone 4's real command API will
// need to define an explicit "back to mirror" exit; this is provisional
// until then.
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
// CPU cycle each) to grab a mirror snapshot. Placeholder -- needs empirical
// tuning on real hardware, same as this file's other WAIT_CYCLE_*/TIMING_*
// constants; picked to be roughly once a second assuming ~1 pass/cycle at
// ~1MHz, unverified.
#define GPU64_MIRROR_POLL_INTERVAL 1000000

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

			register u8 addr = IO_ADDRESS & 0x1f;

			if ( ( IO2_ACCESS && addr == 0x01 && BITS_ALL_SET( D, REU_COMMAND_EXECUTE | REU_COMMAND_FF00_DISABLED ) )
					|| writeFF00 )
			{
				if ( !writeFF00 ) reu.command = D;
				#include "handle_transfer.h"
				reu.reuWaitForFF00 = 0;
			} else
			if ( IO2_ACCESS  )
			{
				register u8 addr = IO_ADDRESS & 0x1f;
				//if ( IO_ADDRESS < 0x0b )
				{
					switch ( addr )
					{
					default:
						// gpu64: unused remainder of IO2 ($DF0B-$DFFF,
						// masked down to 0x0B-0x1F by the "& 0x1f" above --
						// REU only ever decodes 0x00-0x0A) is gpu64's own
						// register window. Milestone 2's trigger register:
						// any write to $DF0B (addr 0x0B) kicks off the
						// checkerboard test pattern. The C64 is DMA-halted
						// for the whole REU polling session (see
						// reuUsingPolling()'s SET_GPIO(bDMA_OUT)), so the
						// real-time cost of drawing here (same call already
						// proven safe as a one-off at boot, see
						// rad_main.cpp/tee_device.h) only delays the
						// still-frozen CPU, it doesn't corrupt any bus
						// timing. See docs/project_description.md's IO
						// address space allocation section.
						if ( addr == 0x0B )
						{
							gpu64ApiActive = 1;
							gpu64_showTestPattern( g_pRAD );
						}
						break;
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
				register u8 addr = IO_ADDRESS & 0x1f;

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

				//if ( IO_ADDRESS >= 0x0b ) D = 0xff;

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