/*
 gpu64 milestone 6 -- core 1's render loop, and the ring buffer both cores
 talk through. See gpu64_3d.h and docs/milestone6_3d_design.md.

 Phase 0: the loop drains commands and counts them. It renders nothing yet
 -- the point of this phase is to prove the two cores talk without core 0's
 bus timing moving, and that is a thing you can only prove with the traffic
 stripped down to nothing else.
*/
#include "gpu64_3d_internals.h"
#include "gpu64_api.h"
#include <circle/util.h>

#ifdef GPU64_3D_ENABLED

Gpu64_3dRing        gpu64_3dRing;
Gpu64_3dWorkerStats gpu64_3dWorkerStats;
Gpu64_3dHostStats   gpu64_3dHost;

// --- the arena ----------------------------------------------------------

static u8  s_Arena[ GPU64_3D_ARENA_BYTES ] __attribute__(( aligned( 64 ) ));
static u32 s_ArenaUsed;

void *gpu64_3dArenaAlloc( u32 nBytes )
{
	// Every allocation starts on a cache line. Wasteful at the byte level and
	// worth it: a texture whose rows share a line with the end of a mesh is a
	// false-sharing bug waiting for the frame core 1 and core 0 happen to
	// touch both in.
	nBytes = ( nBytes + 63 ) & ~63u;

	if ( nBytes > GPU64_3D_ARENA_BYTES - s_ArenaUsed )
		return 0;					// caller answers OUT_OF_MEMORY

	void *p = s_Arena + s_ArenaUsed;
	s_ArenaUsed += nBytes;
	return p;
}

void gpu64_3dArenaReset( void )
{
	// Bump allocator: reset is the whole free path. Nothing is zeroed --
	// every allocation is written before it is read, and zeroing 32 MB here
	// would be the longest unbroken store burst in the firmware.
	s_ArenaUsed = 0;
}

u32 gpu64_3dArenaUsed( void )
{
	return s_ArenaUsed;
}

// --- core 0 side --------------------------------------------------------

void gpu64_3dInit( void )
{
	memset( &gpu64_3dRing, 0, sizeof( gpu64_3dRing ) );
	memset( (void *)&gpu64_3dWorkerStats, 0, sizeof( gpu64_3dWorkerStats ) );
	memset( &gpu64_3dHost, 0, sizeof( gpu64_3dHost ) );
	gpu64_3dArenaReset();
}

void gpu64_3dReset( void )
{
	// A session reset has to leave the ring empty, and only core 0 can say
	// so: core 1 is mid-drain and would happily execute commands belonging to
	// the program that just died. Moving the tail forward from core 0 is the
	// one place that rule is broken, and it is safe precisely because the C64
	// is halted and no new command can arrive while it happens.
	gpu64_3dRing.tail = gpu64_3dRing.head;
	gpu64_3dRing.tailCache = gpu64_3dRing.head;

	// Every resource of the session goes with it -- design doc, Resource
	// lifecycle. Without this a RUN/STOP+RESTORE leaks the whole arena.
	gpu64_3dArenaReset();

	memset( &gpu64_3dHost, 0, sizeof( gpu64_3dHost ) );
}

// Pushes one command onto the ring. Returns FALSE if it is full -- never
// waits, per the design's QUEUE_FULL rule.
static boolean ringPush( u8 op )
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

u8 gpu64_3dDispatch( u8 op )
{
	// Phase 0 accepts exactly the opcodes it can prove something about: the
	// no-argument system ops. Everything else is BAD_OPCODE until the phase
	// that implements it lands. A class that silently accepts what it cannot
	// do is worse than one that rejects it -- the C64 side gets no signal and
	// debugs the wrong layer.
	switch ( op )
	{
	case GPU64_3D_OP_SCENE_RESET:
	case GPU64_3D_OP_LOOP_STOP:
		break;

	default:
		gpu64_3dHost.badOpcode++;
		return GPU64_ERR_BAD_OPCODE;
	}

	if ( !ringPush( op ) )
	{
		gpu64_3dHost.rejected++;
		return GPU64_ERR_QUEUE_FULL;
	}

	gpu64_3dHost.pushed++;
	return GPU64_ERR_OK;
}

char *gpu64_3dReport( char *p )
{
	static const char hex[] = "0123456789ABCDEF";

	// Deliberately terse: this goes into the HDMI log overlay, which is 40
	// columns.
	const char *pLabel = "3D psh/rej/bad ";
	while ( *pLabel )
		*p++ = *pLabel++;

	u32 v[ 3 ] = { gpu64_3dHost.pushed, gpu64_3dHost.rejected, gpu64_3dHost.badOpcode };
	for ( unsigned i = 0; i < 3; i++ )
	{
		if ( i ) *p++ = '/';
		*p++ = hex[ ( v[ i ] >> 12 ) & 0xf ];
		*p++ = hex[ ( v[ i ] >>  8 ) & 0xf ];
		*p++ = hex[ ( v[ i ] >>  4 ) & 0xf ];
		*p++ = hex[   v[ i ]         & 0xf ];
	}

	*p++ = ' ';
	*p++ = 'c';
	*p++ = hex[ ( gpu64_3dWorkerStats.consumed >> 12 ) & 0xf ];
	*p++ = hex[ ( gpu64_3dWorkerStats.consumed >>  8 ) & 0xf ];
	*p++ = hex[ ( gpu64_3dWorkerStats.consumed >>  4 ) & 0xf ];
	*p++ = hex[   gpu64_3dWorkerStats.consumed         & 0xf ];

	return p;
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

// Executes one drained command. Phase 0: counts it. Every later phase adds
// cases here, and every case that writes memory owes GPU64_3D_YIELD() every
// GPU64_3D_SPAN_BYTES.
static void execute( const Gpu64_3dCmd *pCmd )
{
	gpu64_3dWorkerStats.lastOp = pCmd->op;

	switch ( pCmd->op )
	{
	case GPU64_3D_OP_SCENE_RESET:
	case GPU64_3D_OP_LOOP_STOP:
		// Nothing to reset or stop yet -- the scene graph is phase 2 and the
		// loop is phase 4. Accepted so the phase 0 bench has a command that
		// completes end to end.
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

			// Publishing the tail per command rather than per batch keeps a
			// producer that is filling the ring from stalling on a consumer
			// that is draining it.
			gpu64_3dRing.tail = tail;
			gpu64_3dWorkerStats.consumed++;
		}
	}
}

#endif	// GPU64_3D_ENABLED
