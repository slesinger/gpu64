/*

  {_______            {_          {______
        {__          {_ __               {__
        {__         {_  {__               {__
     {__           {__   {__               {__
 {______          {__     {__              {__
       {__       {__       {__            {__
         {_________         {______________		Expansion Unit

 gpu64 milestone 6 -- the multicore memory-traffic load ladder.

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

*/
#include "gpu64_ladder.h"

#ifdef GPU64_LADDER_ENABLED

Gpu64LadderFromWorker gpu64LadderFromWorker;
Gpu64LadderToWorker   gpu64LadderToWorker;

Gpu64LadderStats gpu64Ladder[ GPU64_LADDER_RUNGS + 1 ] __attribute__(( aligned( 64 ) ));
Gpu64LadderPass  gpu64LadderPass;

// One rung per row. `setBytes` is the working-set footprint (the prefix of
// s_Buffer this rung cycles through) and `bytesPerMs` is the throttle: core 1
// writes that many bytes, then sleeps out the rest of the millisecond against
// the generic timer, so the rate is set by wall clock rather than by how fast
// the core happens to be. Bytes-per-millisecond is also megabytes-per-second
// times 1000, which is why the achieved-rate column below is just bytes/us.
//
// See gpu64_ladder.h for how the axis got here. In short: rate turned out not
// to be a variable at all, so this table holds it fixed and sweeps footprint
// upward until core 0 breaks. Where that happens is milestone 6's budget.
static const struct
{
	const char *pLabel;			// 7 characters, padded
	u32	setBytes;
	u32	bytesPerMs;
	u8	unthrottled;
	u8	nonTemporal;
}
s_Rung[ GPU64_LADDER_RUNGS + 1 ] =
{
	{ "idle   ",	0,						0,							0, 0 },

	// Ascending footprint at a fixed 64 MB/s. 16K is round 5's most-proven-
	// clean load; tight spacing through 512K (the shared L2's size) because
	// that is where the answer has to be.
	{ "16K    ",	GPU64_LADDER_SET_16K,	GPU64_LADDER_SWEEP_RATE,	0, 0 },
	{ "64K    ",	GPU64_LADDER_SET_64K,	GPU64_LADDER_SWEEP_RATE,	0, 0 },
	{ "128K   ",	GPU64_LADDER_SET_128K,	GPU64_LADDER_SWEEP_RATE,	0, 0 },
	{ "192K   ",	GPU64_LADDER_SET_192K,	GPU64_LADDER_SWEEP_RATE,	0, 0 },
	{ "256K   ",	GPU64_LADDER_SET_256K,	GPU64_LADDER_SWEEP_RATE,	0, 0 },
	{ "320K   ",	GPU64_LADDER_SET_320K,	GPU64_LADDER_SWEEP_RATE,	0, 0 },
	{ "384K   ",	GPU64_LADDER_SET_384K,	GPU64_LADDER_SWEEP_RATE,	0, 0 },
	{ "448K   ",	GPU64_LADDER_SET_448K,	GPU64_LADDER_SWEEP_RATE,	0, 0 },
	{ "512K   ",	GPU64_LADDER_SET_512K,	GPU64_LADDER_SWEEP_RATE,	0, 0 },
	{ "768K   ",	GPU64_LADDER_SET_768K,	GPU64_LADDER_SWEEP_RATE,	0, 0 },
	{ "1M     ",	GPU64_LADDER_SET_1M,	GPU64_LADDER_SWEEP_RATE,	0, 0 },
	{ "2M     ",	GPU64_LADDER_SET_DRAM,	GPU64_LADDER_SWEEP_RATE,	0, 0 },

	{ "done   ",	0,						0,							0, 0 }
};

// Core 1's working set, sized for the largest arm. Static rather than
// heap-allocated for the same reason the spike's was: this is a diagnostic
// build and the allocator is not part of the question. Cache-line aligned so
// the L1 arm's 16KB really is 16KB of whole lines and not 16KB plus two
// straddling neighbours.
static u64 s_Buffer[ GPU64_LADDER_BUFFER_BYTES / 8 ] __attribute__(( aligned( 64 ) ));

// Bytes core 1 actually managed to write in each rung, and the microseconds
// it spent there. Written only by core 1, read only after the ladder is
// over -- the achieved rate is what the report prints, because a throttle
// that could not keep up would otherwise silently mislabel a rung.
static volatile u64 s_Written[ GPU64_LADDER_RUNGS + 1 ] __attribute__(( aligned( 64 ) ));
static volatile u64 s_ElapsedTicks[ GPU64_LADDER_RUNGS + 1 ] __attribute__(( aligned( 64 ) ));

void gpu64_ladderArm( void )
{
	for ( unsigned i = 0; i <= GPU64_LADDER_RUNGS; i++ )
	{
		gpu64Ladder[ i ].passes     = 0;
		gpu64Ladder[ i ].lost       = 0;
		gpu64Ladder[ i ].lost2      = 0;
		gpu64Ladder[ i ].minDelta   = 0xFFFFFFFF;
		gpu64Ladder[ i ].maxDelta   = 0;
		gpu64Ladder[ i ].totalDelta = 0;
		s_Written[ i ]      = 0;
		s_ElapsedTicks[ i ] = 0;
	}

	gpu64LadderPass.prev       = 0;
	gpu64LadderPass.skip       = 1;
	gpu64LadderPass.tick       = 0;
	gpu64LadderPass.rung       = 0;
	gpu64LadderPass.threshold  = 0;
	gpu64LadderPass.threshold2 = 0;
	gpu64LadderPass.baseline   = 0;
	gpu64LadderPass.dumped     = 0;

	gpu64LadderFromWorker.rung = 0;

	// Bump last, and after everything else is cleared: this is what core 1
	// watches, and seeing it move is what makes core 1 restart its own
	// timeline. Before round 7 there was no generation counter and core 1
	// latched its start time exactly once, so a second arming -- which
	// happens on every C64 reset, since resetREU() clears gpu64ApiActive and
	// the next GET_INFO re-arms -- left core 1 running the *original*
	// timeline. Round 6 hit this: after a transient derail at 2s the ladder
	// re-armed, core 1's clock was already past the last rung, and it
	// published `done` for the remaining 51 seconds. The table came back with
	// every rung at zero and `done p=49016k`.
	gpu64LadderToWorker.start = 1;
	gpu64LadderToWorker.gen++;
}

// The ARM generic timer, read as a *system register*. Not
// read32( ARM_SYSTIMER_CLO ).
//
// This distinction cost the first bench round (2026-08-23). The BCM system
// timer is MMIO on the VideoCore peripheral bus -- the same bus core 0 reads
// ARM_GPIO_GPLEV0 from, in a tight spin, as its entire notion of where the
// C64's clock is. Core 1 polling that register to run its throttle put a
// second maximum-rate reader on that bus and pushed core 0's GPIO reads
// late. CNTVCT_EL0 is core-local silicon: no bus cycle, no contention, and
// no way for the instrument to disturb the thing it is measuring.
static inline u64 ladderTicks( void )
{
	u64 v;
	asm volatile( "MRS %0, CNTVCT_EL0" : "=r" (v) );
	return v;
}

static inline u64 ladderFreq( void )
{
	u64 v;
	asm volatile( "MRS %0, CNTFRQ_EL0" : "=r" (v) );
	return v;
}

// Put core 1 genuinely to sleep between slots, instead of spinning.
//
// Round 2 (2026-08-23) corrupted core 0 with core 1 writing zero bytes and
// touching no MMIO -- a `yield` spin was enough, and `yield` on a
// Cortex-A53 is a hint that does nothing. Round 3, with core 1 parked in
// WFI, was flawless. So the spin itself is a disturbance, and leaving it in
// would make it a confound in *every* rung: each rung would measure its
// write traffic plus a constant spin, and the ladder is supposed to vary
// one thing.
//
// The generic timer's event stream is the way to wait without spinning and
// without interrupts: CNTKCTL_EL1.EVNTEN makes a chosen counter bit
// generate events, and WFE sleeps until one arrives. EVNTI = 14 gives an
// event every 2^14 ticks = 853us at 19.2MHz -- fine against a 1ms slot,
// since the loops re-check the real deadline on every wake and simply sleep
// again. No interrupt is involved at any point, which matters because RAD
// runs with IRQs disabled and a secondary core taking an unexpected
// exception has nowhere to report it.
//
// Failure mode if the event stream does not work: core 1 sleeps forever,
// the rung index never advances, and the report says so plainly -- every
// rung past idle reads p=0 and RATE reads all zeros. Visible, and harmless
// to the C64.
#define GPU64_LADDER_EVNTI	14

static inline void ladderEnableEventStream( void )
{
	u64 v;
	asm volatile( "MRS %0, CNTKCTL_EL1" : "=r" (v) );
	v &= ~( (u64)0xF << 4 );			// EVNTI
	v |=  ( (u64)GPU64_LADDER_EVNTI << 4 );
	v &= ~( (u64)1 << 3 );				// EVNTDIR = 0, count up
	v |=  ( (u64)1 << 2 );				// EVNTEN
	asm volatile( "MSR CNTKCTL_EL1, %0" :: "r" (v) );
	asm volatile( "ISB" );
}

// Non-temporal store pair. STNP is the AArch64 hint that this data has no
// reuse: it writes without allocating in L1 or L2, so the stream does not
// evict anything either core is relying on. That is precisely the mechanism
// round 4 implicates, which is what makes the NT rung a test of the
// diagnosis and not just a second data point.
//
// Two u64s per instruction, hence the pairwise loop and the even word counts
// the rung table's byte figures all happen to give.
static inline void ladderStoreNT( u64 *p, u64 v )
{
	asm volatile( "STNP %1, %2, [%0]" :: "r" (p), "r" (v), "r" (v) : "memory" );
}

void gpu64_ladderWorker( void )
{
	// volatile so the ordinary stores are real traffic and not something the
	// optimiser can prove nobody reads. The non-temporal path goes through
	// inline asm and does not need it.
	volatile u64 *pBuf = s_Buffer;

	u32 idx = 0;
	u64 v   = 0;

	ladderEnableEventStream();

	// Also part of the first round's damage: this used to be a bare spin,
	// and it ran from boot until the first gpu64 command -- through all of
	// the RAD menu and the BASIC LOAD. Now the core is actually asleep for
	// all of it.
	u32 nGen = gpu64LadderToWorker.gen;
	while ( nGen == 0 )
		{ asm volatile( "WFE" ); nGen = gpu64LadderToWorker.gen; }

	// The armstub's EL3 prologue programs CNTFRQ_EL0 to OSC_FREQ on every
	// core (Source/Firmware/ARM STUB/rad-prefetch.S), so this is 19.2MHz in
	// practice -- but a zero here would be a divide-by-zero hang on a core
	// nothing can log from, so it falls back rather than trusting that.
	u64 nFreq = ladderFreq();
	if ( nFreq == 0 )
		nFreq = 19200000;

	// Multiply before dividing. This read `nFreq * ( GPU64_LADDER_RUNG_US /
	// 1000000 )` until round 6, where GPU64_LADDER_RUNG_US was 2500000 and
	// integer division silently made every rung 2 seconds instead of 2.5 --
	// visible only as an idle rung with 1920k passes where 2400k was
	// expected. 19.2e6 * 2e6 is 3.8e13, comfortably inside u64.
	const u64 nRungTicks = nFreq * GPU64_LADDER_RUNG_US / 1000000;
	const u64 nSlotTicks = nFreq / 1000;

	u64 tStart = ladderTicks();
	u64 tSlot  = tStart;
	u64 tPrev  = tStart;
	u32 nLastSet = 0;

	u64 nAccBytes = 0;
	u64 nAccTicks = 0;
	u32 nAccSlots = 0;
	u32 nAccRung  = 0;

	while ( 1 )
	{
		u64 now = ladderTicks();

		// Re-armed: start the ladder over from rung 0 against a fresh clock.
		if ( gpu64LadderToWorker.gen != nGen )
		{
			nGen = gpu64LadderToWorker.gen;
			tStart = tSlot = tPrev = now;
			nLastSet = 0;
			idx = 0;
			nAccBytes = nAccTicks = 0;
			nAccSlots = 0;
			nAccRung  = 0;
		}

		u64 rung = ( now - tStart ) / nRungTicks;
		if ( rung > GPU64_LADDER_RUNGS )
			rung = GPU64_LADDER_RUNGS;
		gpu64LadderFromWorker.rung = (u32)rung;

		const u32 nBytes = s_Rung[ rung ].bytesPerMs;
		const u32 nWrite = nBytes / 8;
		const u32 nWords = s_Rung[ rung ].setBytes / 8;

		// Restart at the top of the buffer whenever the footprint changes, so
		// a rung never inherits a cursor pointing past its own working set.
		if ( s_Rung[ rung ].setBytes != nLastSet )
			{ nLastSet = s_Rung[ rung ].setBytes; idx = 0; }

		if ( nWords )
		{
			if ( s_Rung[ rung ].nonTemporal )
			{
				for ( u32 i = 0; i + 1 < nWrite; i += 2 )
				{
					ladderStoreNT( (u64 *)&pBuf[ idx ], v++ );
					idx += 2;
					if ( idx + 1 >= nWords ) idx = 0;
				}
			} else
			{
				for ( u32 i = 0; i < nWrite; i++ )
				{
					pBuf[ idx ] = v++;
					if ( ++idx >= nWords ) idx = 0;
				}
			}
		}

		// Wall time is attributed a whole slot at a time, throttle wait
		// included -- the achieved rate has to be the rung's *average*
		// rate, which is what the throttle sets, not the rate of the burst
		// inside each millisecond.
		//
		// Accumulated in registers and published rarely rather than written
		// every slot. These arrays are on their own cache lines now, so a
		// per-slot write would no longer be able to reach core 0's state --
		// but 1000 shared-memory writes a second, from the core whose entire
		// job is to not disturb core 0, is not a thing to leave in on the
		// grounds that it is probably harmless.
		nAccBytes += nBytes;
		nAccTicks += now - tPrev;
		tPrev = now;

		if ( rung != nAccRung || ++nAccSlots >= 64 )
		{
			s_Written[ nAccRung ]      += nAccBytes;
			s_ElapsedTicks[ nAccRung ] += nAccTicks;
			nAccBytes = nAccTicks = 0;
			nAccSlots = 0;
			nAccRung  = (u32)rung;
		}

		u64 after = ladderTicks();

		if ( s_Rung[ rung ].unthrottled )
		{
			tSlot = after;
			continue;
		}

		tSlot += nSlotTicks;

		// If the rung's budget did not fit in its millisecond, do not let
		// the throttle turn into a catch-up burst -- resync and let the
		// achieved-rate column report the shortfall instead.
		if ( (s64)( after - tSlot ) > 0 )
			{ tSlot = after; continue; }

		while ( (s64)( ladderTicks() - tSlot ) < 0 )
			asm volatile( "WFE" );
	}
}

static char *ladDec( char *p, u32 v )
{
	char tmp[ 10 ];
	unsigned n = 0;
	do { tmp[ n++ ] = (char)( '0' + v % 10 ); v /= 10; } while ( v != 0 );
	while ( n-- ) *p++ = tmp[ n ];
	return p;
}

static char *ladStr( char *p, const char *q )
{
	while ( *q ) *p++ = *q++;
	return p;
}

char *gpu64_ladderReport( char *p )
{
	// Header: the calibration. `c64=` is the *mean* ARM cycles between two
	// consecutive undisturbed loop passes -- one C64 cycle, and a sanity
	// check in its own right: 1421 at the 1400MHz config.txt pins. `thr=`
	// is 1.5x that, the line above which a pass took longer than one C64
	// cycle. See the note on GPU64_LADDER_CALIB_PASSES for why this is the
	// mean and not the minimum; round 3 was scored 99.5% failing by a
	// minimum-based threshold on a flawless run.
	p = ladStr( p, "LADDER c64=" );
	p = ladDec( p, gpu64LadderPass.baseline );
	p = ladStr( p, " thr=" );
	p = ladDec( p, gpu64LadderPass.threshold );
	*p++ = '\n';

	for ( unsigned i = 0; i <= GPU64_LADDER_RUNGS; i++ )
	{
		const Gpu64LadderStats *s = &gpu64Ladder[ i ];

		p = ladStr( p, s_Rung[ i ].pLabel );
		*p++ = ' ';

		p = ladStr( p, "p=" );
		p = ladDec( p, s->passes / 1000 );
		*p++ = 'k';

		// Two severities: past 4/3 of a C64 cycle, and past two of them. See
		// the note on the threshold in gpu64_ladder.h -- the pair is what
		// keeps this round comparable with the ones scored at 3/2.
		p = ladStr( p, " L=" );
		p = ladDec( p, s->lost );
		*p++ = '/';
		p = ladDec( p, s->lost2 );

		// Per-rung mean and worst case. The mean is here because round 3
		// showed it is the number that says whether a rung was healthy at
		// all: a rung whose mean has drifted off the calibrated c64= figure
		// is one where core 0 is systematically late, which is a different
		// and worse thing than a few long passes.
		p = ladStr( p, " m=" );
		p = ladDec( p, s->passes ? (u32)( s->totalDelta / s->passes ) : 0 );

		p = ladStr( p, " x=" );
		p = ladDec( p, s->maxDelta );

		*p++ = '\n';
	}

	// Achieved rates, MB/s, one field per rung in ladder order. On its own
	// line because the per-rung lines are already at the log's 40 columns,
	// and because what these are for is validating the throttle rather than
	// reading alongside the timing: a rung whose achieved rate falls short
	// of its label was really running at this number, not at its name.
	//
	// Bytes per microsecond is megabytes per second. Generic-timer ticks
	// are converted here rather than per slot, so the worker never does a
	// 64-bit division in its inner loop.
	p = ladStr( p, "RATE" );
	for ( unsigned i = 0; i <= GPU64_LADDER_RUNGS; i++ )
	{
		u64 nHz = ladderFreq();
		u64 nUs = nHz ? s_ElapsedTicks[ i ] * 1000000 / nHz : 0;

		*p++ = ( i == 0 ) ? ' ' : '/';
		p = ladDec( p, nUs ? (u32)( s_Written[ i ] / nUs ) : 0 );
	}
	*p++ = '\n';

	return p;
}

#endif
