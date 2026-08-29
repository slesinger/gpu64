/*
 gpu64 milestone 6 -- the ring buffer both cores talk through, and core 1's
 drain loop. See gpu64_3d.h and project/milestone6_3d_design.md.

 Through Stage 14, what core 1 did was count: every class 1 opcode ran
 synchronously on core 0, inside the dispatch window where the C64 is already
 DMA-halted, and the ring only kept the cross-core path under real command
 traffic so the timing question milestone 6a opened stayed measurable while
 the pipeline was built.

 Stage 15a (project/gap_filling_plan.md) gives execute() below a real job for
 three opcodes: CLEAR_VIEWPORT, DRAW_MESH and DRAW_NODE now run for real on
 core 1, via gpu64_3dExecuteRender() (gpu64_3d_class1.cpp, which still owns
 all the session state this needs and cannot export). gpu64_3dDispatch()
 stalls the whole dispatch on the ring fully draining before it returns --
 15a's zero-concurrency window -- so there is still exactly one owner of the
 framebuffer and the z-buffer at any instant, just relocated from "core 0,
 always" to "core 1, while core 0 waits". Every other class 1 opcode is
 unchanged: pushed for the ring's own counters, then still executed
 synchronously on core 0.
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

// Records one drained command, and, for the three render opcodes, actually
// executes it. pCmd is non-const now: gpu64_3dExecuteRender() writes its
// result back into the slot for gpu64_3dDispatch() to read once its drain
// wait succeeds.
//
// Every opcode besides the three render ones is still legal here with no
// handler, because core 0 still executes them synchronously, itself, before
// this drain loop ever sees them go by -- this function's count is the only
// trace they leave on core 1. That is why unknownOp below is not a fault
// counter: it is expected to climb continuously, on every non-render class 1
// command, and gpu64_3dReport() (gpu64_3d_class1.cpp) does not treat it as
// one.
static void execute( Gpu64_3dCmd *pCmd )
{
	gpu64_3dWorkerStats.lastOp = pCmd->op;

	switch ( pCmd->op )
	{
	case GPU64_3D_OP_CLEAR_VIEWPORT:
	case GPU64_3D_OP_DRAW_MESH:
	case GPU64_3D_OP_DRAW_NODE:
		gpu64_3dExecuteRender( pCmd );
		break;

	default:
		gpu64_3dWorkerStats.unknownOp++;
		break;
	}
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

			// For the three render ops, execute() just wrote pCmd->err/result
			// into the slot gpu64_3dDispatch() is stalled waiting to read back
			// (waitForDrain(), gpu64_3d_class1.cpp). Those writes must be
			// visible before the tail that tells core 0 to go read them --
			// pairs with the DMB core 0 issues after observing tail == head.
			asm volatile( "DMB ISH" ::: "memory" );

			// Publishing the tail per command rather than per batch keeps a
			// producer that is filling the ring from stalling on a consumer
			// that is draining it.
			gpu64_3dRing.tail = tail;
			gpu64_3dWorkerStats.consumed++;
		}
	}
}

#endif	// GPU64_3D_ENABLED
