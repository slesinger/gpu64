/*
 gpu64 milestone 6 -- class 1 internals: the core-0-to-core-1 ring buffer,
 the resource arena's bump allocator, and the store-burst yield.

 Private to the gpu64_3d_*.cpp files. The public interface is gpu64_3d.h.
*/
#ifndef _gpu64_3d_internals_h
#define _gpu64_3d_internals_h

#include "gpu64_3d.h"
#include <circle/types.h>

#ifdef GPU64_3D_ENABLED

// --- the store-burst budget ---------------------------------------------
//
// Milestone 6a's result, in one constant. Core 1 may write at most 7
// consecutive cache lines (448 bytes) before yielding -- 8 if the target is
// cache-resident -- or core 0's PHI-locked polling loop misses its deadline
// and the C64 derails. Rate is not an axis and neither is working-set size;
// only the length of an unbroken run of stores is. Full measurements:
// docs/milestone6_3d_design.md, "The real risk: store bursts".
//
// 256 is the design figure rather than 448: it is 57% of the measured edge,
// it was clean at every rate tested, and it maps onto a natural unit -- a
// 320-pixel 8bpp scanline is 320 bytes, so one yield per scanline (or two)
// falls out of the geometry instead of being bolted on.
#define GPU64_3D_SPAN_BYTES		256

// gpu64: the yield itself. What milestone 6a actually measured was the burst
// *length* at which core 0 breaks -- the ladder's own pacing supplied the gap
// between bursts, so the cheapest sufficient barrier is NOT yet a measured
// quantity. DSB is the conservative choice: it retires the outstanding stores
// before the next span starts, which is the property the budget assumes.
//
// OPEN ITEM for phase 3: bench a DMB, and a plain nothing-at-all, against a
// real REU round-trip. If a weaker barrier holds, the rasteriser's inner loop
// gets measurably cheaper. Do not weaken this on reasoning alone -- three
// published diagnoses in milestone 6a were killed by later rounds.
#define GPU64_3D_YIELD()	asm volatile( "DSB ISH" ::: "memory" )

// --- the command ring ---------------------------------------------------
//
// One-way, core 0 producer, core 1 consumer. 32 bytes an entry so the whole
// 16-byte ARG block travels inline: no second allocation, no pointer into
// storage core 0 would then have to keep alive, and two entries to a cache
// line.
//
// The ARG copy is what makes the ring safe against the C64 overwriting ARG
// for its next command while core 1 is still working on this one. Blob
// *payloads* are a phase 1 problem and go through the arena; the descriptor
// pointing at them fits here.
struct Gpu64_3dCmd
{
	u8	op;
	u8	flags;
	u16	id;			// the staged ID at the moment of dispatch
	u32	aux;			// phase 1: arena offset of a pulled blob
	u32	auxLen;
	u32	pad;
	u8	arg[ 16 ];
};

#define GPU64_3D_RING_ENTRIES	256		// 8 KB of ring
#define GPU64_3D_RING_MASK	( GPU64_3D_RING_ENTRIES - 1 )

// gpu64: head and tail each alone on a cache line, and each written by
// exactly one core. This is the false-sharing rule from milestone 6a round
// 10 -- separating the lines was not enough there; what took the noise floor
// to zero was core 0 not *reading* a line core 1 writes. Core 0 still has to
// know where the tail is to detect a full ring, so it keeps a private cached
// copy (headCache/tailCache below) and only re-reads the real one when that
// copy says full. On a ring that is not full, core 0 never touches core 1's
// line at all.
//
// Dispatch runs with the C64 DMA-halted and has no deadline, so a coherence
// miss here is survivable in a way one in the polling loop is not -- but the
// cached-tail form costs nothing, so there is no reason to spend it.
struct Gpu64_3dRing
{
	// written by core 0, read by core 1
	volatile u32	head;
	u32		pad0[ 15 ];

	// written by core 1, read by core 0 (rarely -- see above)
	volatile u32	tail;
	u32		pad1[ 15 ];

	// core-0-private: last tail value core 0 bothered to read.
	u32		tailCache;
	u32		pad2[ 15 ];

	Gpu64_3dCmd	slot[ GPU64_3D_RING_ENTRIES ];
} __attribute__(( aligned( 64 ) ));

extern Gpu64_3dRing gpu64_3dRing;

// --- phase 0 counters ---------------------------------------------------
//
// The only output phase 0 has. Core 1 writes these and core 0 reads them
// once, from the log path, which is not the hot loop -- so the read is
// allowed here in a way it would not be inside reuUsingPolling().
struct Gpu64_3dWorkerStats
{
	volatile u32	consumed;	// commands drained off the ring
	volatile u32	lastOp;		// most recent opcode core 1 saw
	volatile u32	unknownOp;	// opcodes core 1 has no handler for yet
	volatile u32	wakeups;	// times the drain loop found work
	u32		pad[ 12 ];
} __attribute__(( aligned( 64 ) ));

extern Gpu64_3dWorkerStats gpu64_3dWorkerStats;

// Core-0-side counters, core-0-private, so they share no line with the above.
struct Gpu64_3dHostStats
{
	u32	pushed;			// commands accepted onto the ring
	u32	rejected;		// QUEUE_FULL
	u32	badOpcode;
	u32	pad[ 13 ];
} __attribute__(( aligned( 64 ) ));

extern Gpu64_3dHostStats gpu64_3dHost;

// --- the resource arena -------------------------------------------------
//
// A bump allocator over one static block, per docs/milestone6_3d_design.md's
// Resource lifecycle: explicit free only, no automatic eviction, and the
// whole thing is reclaimed on resetREU(). Phase 0 only brings it up; phase 1
// is what actually allocates out of it.
//
// 32 MB against a C64/REU-side source ceiling of 16 MB, so a program cannot
// fill it from the C64 side in one session even by uploading every byte it
// can address. Static rather than heap: the same reason gpu64_api.cpp stages
// blobs statically -- keeping the memory allocator out of a path core 1
// runs.
#define GPU64_3D_ARENA_BYTES	( 32 * 1024 * 1024 )

// Owned by core 1. Core 0 never allocates -- it pushes a command and core 1
// does the work -- so there is no lock here, and deliberately so: a lock core
// 0 could block on would be exactly the thing the QUEUE_FULL rule exists to
// prevent.
void *gpu64_3dArenaAlloc( u32 nBytes );
void  gpu64_3dArenaReset( void );
u32   gpu64_3dArenaUsed( void );

#endif	// GPU64_3D_ENABLED

#endif
