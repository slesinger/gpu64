/*
 gpu64 milestone 6 -- the store-burst budget and the yield that enforces it.

 Split out of gpu64_3d_internals.h so the portable renderer (which knows
 nothing about the ring buffer, core 1, or Circle) can obey the same budget
 and still compile natively for tools/hostsim.

 Milestone 6a's result, in one constant. A core other than core 0 may write
 at most 7 consecutive cache lines (448 bytes) before yielding, or core 0's
 PHI-locked polling loop misses its deadline and the C64 derails. Rate is not
 an axis and neither is working-set size; only the length of an unbroken run
 of stores is. Full measurements: docs/milestone6_3d_design.md, "The real
 risk: store bursts".
*/
#ifndef _gpu64_3d_span_h
#define _gpu64_3d_span_h

// 256 is the design figure rather than 448: it is 57% of the measured edge,
// it was clean at every rate tested, and it maps onto a natural unit -- a
// 320-pixel 8bpp scanline is 320 bytes, so one yield per scanline (or two)
// falls out of the geometry instead of being bolted on.
#define GPU64_3D_SPAN_BYTES		256

#ifdef GPU64_HOSTSIM

// The host sim is measuring pixels, not bus timing. A barrier here would
// cost nothing but noise.
#define GPU64_3D_YIELD()		( (void)0 )

#else

// gpu64: what milestone 6a actually measured was the burst *length* at which
// core 0 breaks -- the ladder's own pacing supplied the gap between bursts,
// so the cheapest sufficient barrier is NOT yet a measured quantity. DSB is
// the conservative choice: it retires the outstanding stores before the next
// span starts, which is the property the budget assumes.
//
// OPEN ITEM: bench a DMB, and a plain nothing-at-all, against a real REU
// round-trip. If a weaker barrier holds, the rasteriser's inner loop gets
// measurably cheaper. Do not weaken this on reasoning alone -- three
// published diagnoses in milestone 6a were killed by later rounds.
#define GPU64_3D_YIELD()		asm volatile( "DSB ISH" ::: "memory" )

#endif

#endif
