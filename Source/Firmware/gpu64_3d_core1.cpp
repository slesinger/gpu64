/*
 gpu64 milestone 6 -- the ring buffer both cores talk through, and core 1's
 drain loop. See gpu64_3d.h and project/milestone6_3d_design.md.

 What core 1 does today is count. The renderer that phase 1 built runs on
 core 0, synchronously, inside the dispatch window where the C64 is already
 DMA-halted -- exactly like every class 0 draw op (gpu64_3d_class1.cpp says
 why). So this file is not yet the render loop; it is the instrument that
 keeps the cross-core path under real command traffic, so the timing question
 milestone 6a opened stays measurable while the pipeline is built.
*/
#include "gpu64_3d_internals.h"
#include "gpu64_api.h"
#include <circle/util.h>

#ifdef GPU64_3D_ENABLED

Gpu64_3dRing        gpu64_3dRing;
Gpu64_3dWorkerStats gpu64_3dWorkerStats;
Gpu64_3dHostStats   gpu64_3dHost;

// Pushes one command onto the ring. Returns FALSE if it is full -- never
// waits, per the design's QUEUE_FULL rule. Core 0 side; it lives here rather
// than next to the dispatcher because the barrier below only makes sense read
// against the worker at the bottom of this file.
boolean gpu64_3dRingPush( u8 op )
{
	const u32 head = gpu64_3dRing.head;
	const u32 next = ( head + 1 ) & GPU64_3D_RING_MASK;

	// The cached-tail check (gpu64_3d_internals.h): on a ring that is not
	// full this reads core-0-private state and never touches core 1's line.
	if ( next == gpu64_3dRing.tailCache )
	{
		gpu64_3dRing.tailCache = gpu64_3dRing.tail;	// the one coherence miss
		if ( next == gpu64_3dRing.tailCache )
			return FALSE;
	}

	Gpu64_3dCmd *pSlot = &gpu64_3dRing.slot[ head ];

	pSlot->op     = op;
	pSlot->flags  = 0;
	pSlot->id     = (u16)( gpu64Regs.id[ 0 ] | ( gpu64Regs.id[ 1 ] << 8 ) );
	pSlot->aux    = 0;
	pSlot->auxLen = 0;
	memcpy( pSlot->arg, gpu64Regs.arg, GPU64_ARG_COUNT );

	// The slot must be visible before the head that advertises it. Without
	// this barrier core 1 can observe the new head against the previous
	// occupant of the slot -- a stale command executed with a fresh opcode,
	// which is the kind of bug that reproduces once an hour.
	asm volatile( "DMB ISH" ::: "memory" );

	gpu64_3dRing.head = next;
	return TRUE;
}

// --- core 1 side --------------------------------------------------------

// gpu64: the generic-timer event stream, so WFE wakes on its own instead of
// needing an SEV from core 0. Core 0 executing an SEV per command would put
// a store into its hot path for core 1's benefit, which is the wrong way
// round -- core 1 is the side with slack.
//
// Same mechanism gpu64_ladder.cpp uses; kept separate rather than shared
// because the ladder is a diagnostic build that will eventually be deleted.
#define GPU64_3D_EVNTI		8		// 2^8 ticks ~= 13us at 19.2MHz

static void enableEventStream( void )
{
	u64 v;
	asm volatile( "MRS %0, CNTKCTL_EL1" : "=r" (v) );
	v &= ~( (u64)0xf << 4 );
	v |= ( (u64)GPU64_3D_EVNTI << 4 );
	v |= ( (u64)1 << 2 );				// EVNTEN
	asm volatile( "MSR CNTKCTL_EL1, %0" :: "r" (v) );
	asm volatile( "ISB" );
}

// Records one drained command. Core 1 executes nothing today -- see the file
// header -- so this is deliberately not a switch: every opcode is legal here,
// because core 0 has already validated and executed it. A switch would have
// to be kept in step with core 0's for no gain, and the day core 1 does take
// over the render loop it wants a fresh one written against that phase's
// opcode set, not this one inherited.
static void execute( const Gpu64_3dCmd *pCmd )
{
	gpu64_3dWorkerStats.lastOp = pCmd->op;
}

void gpu64_3dWorker( void )
{
	enableEventStream();

	u32 tail = 0;

	while ( 1 )
	{
		if ( tail == gpu64_3dRing.head )
		{
			// Idle. WFE parks the core until the event stream fires, which
			// costs no bus cycle and no store -- the two things milestone 6a
			// proved core 0 cannot tolerate from another core.
			asm volatile( "WFE" );
			continue;
		}

		gpu64_3dWorkerStats.wakeups++;

		while ( tail != gpu64_3dRing.head )
		{
			// Pairs with the DMB in ringPush(): the head we just read must not
			// let us see the slot's previous contents.
			asm volatile( "DMB ISH" ::: "memory" );

			execute( &gpu64_3dRing.slot[ tail ] );

			tail = ( tail + 1 ) & GPU64_3D_RING_MASK;

			// Publishing the tail per command rather than per batch keeps a
			// producer that is filling the ring from stalling on a consumer
			// that is draining it.
			gpu64_3dRing.tail = tail;
			gpu64_3dWorkerStats.consumed++;
		}
	}
}

#endif	// GPU64_3D_ENABLED
