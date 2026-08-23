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
#ifndef _gpu64_ladder_h
#define _gpu64_ladder_h

#include <circle/types.h>
#include <circle/bcm2835.h>
#include <circle/memio.h>

// gpu64: what this is, and why it is not the 2026-08-22 stress spike.
//
// The spike (gpu64_multicore.h) answered one binary question with a
// deliberately worst-case workload: three cores streaming 2MB buffers flat
// out corrupt the C64's own VIC-II output, while multicore *bring-up* alone
// does not. That leaves milestone 6's actual question open -- the render
// loop does not need flat-out bandwidth, it needs *some*, and nobody knows
// how much is safe. See docs/milestone6_3d_design.md: "find the actual
// contention threshold (a load ladder ... working up rather than starting
// from a deliberately worst-case synthetic stress)".
//
// So: one worker core (not three -- the design says start with core 0 plus
// one worker and expand only once that is measured), stepping through a
// ladder of *throttled* bandwidths, while core 0 measures its own bus-watch
// loop and the C64 runs a real REU + gpu64 workload. Each rung lasts a
// fixed wall-clock window; the whole ladder is one hardware run, and the
// result is a table printed by LOG_ENABLE(1).
//
// The pass/fail number is per-rung `L=`: bus cycles core 0 missed. The
// previous rounds' pass/fail signal was "does the RAD menu look like
// garbage", which cannot rank rungs. This one can.
//
// LEFT ON deliberately, unlike gpu64_multicore.h's toggles: this exists for
// one bench round and the image that round needs is the one tools/build.sh
// produces by default. **Comment it back out once the ladder result is
// recorded in docs/progress_tracker.md.** It forces multicore bring-up on
// (rad_main.h) whatever gpu64_multicore.h's own toggles say, it widens
// reuUsingPolling()'s instruction-cache preload window (below), and it puts
// measurement code inside that loop -- a diagnostic build, never a shipping
// one.
//
// With it commented out, every byte of this file compiles away: verified by
// building both ways and diffing the symbol table -- reuUsingPolling() comes
// out at the same address and the same 0x1b54 bytes as before the ladder
// existed.
#define GPU64_LADDER_ENABLED

// The isolation control the first bench round turned out to need. With this
// commented out, the ladder build keeps *everything* that touches core 0 --
// the per-pass measurement, the widened preload window, multicore bring-up
// -- but core 1 never runs the worker at all and parks in halt()/WFI, which
// is exactly the configuration that tested clean on 2026-08-22. A run in
// that state that still misbehaves indicts the instrument; a clean one
// exonerates it and puts the fault back on core 1. Leave it ON for a normal
// ladder run.
//
// Round 3 (2026-08-23) ran with this OFF and came back **perfectly clean**:
// 2048/2048 frames, zero REU verify errors, ERRCODE 0, a worst-case pass of
// 1716 ARM cycles (1.23us -- never even close to two C64 cycles) across
// 32.14M passes, and a visibly flawless sweep. That exonerates the
// instrument, the widened preload window and multicore bring-up, and leaves
// core 1's *activity* as the only remaining suspect for round 2's
// corruption. It is also the control the ladder now has and did not before.
//
// Back ON since round 4, which answered that question and produced the
// finding the current rung table is built around (see below).
#define GPU64_LADDER_WORKER_ENABLED

// Rungs, in order. Round 8 sweeps footprint upward again, but **starting from
// the most-proven-clean load in the whole series** rather than from 256 K.
//
// Round 7 came back with a result that contradicts round 5 outright. Round 5
// ran 256 KB at an achieved 449 MB/s for 2.9M passes with `L=0`; round 7 ran
// 256 KB at 64 MB/s and scored `L=694` in 49k passes before the machine died.
// Same footprint, same buffer, a *lower* rate, opposite outcome. Worse, round
// 7's `idle` rung -- core 1 asleep, writing nothing, the configuration that
// was flawless in rounds 4, 5 and 6 -- came back `L=30 x=3670` where every
// previous round read `L=0 x=1703`.
//
// A contradiction that includes the idle rung is not a finding about core 1.
// Something about the measurement changed, and until that is understood no
// number from round 7 means anything. So this round is built to isolate it:
//
//   - The sweep starts at 16 KB, which round 5 measured clean at 450 MB/s.
//     If 16 KB is dirty now, the instrument regressed between rounds 6 and 8
//     and that is the whole result. If 16 KB is clean and the dirt starts
//     somewhere below 256 K, then round 5's 256 K figure was order-dependent
//     -- it followed three 16 KB rungs, so core 1's set was already warm --
//     and the real cliff is lower than round 5 implied.
//   - The C64 program no longer touches the cartridge at all (see the test
//     PRG), so `idle` becomes a measurement of core 0 entirely alone. A dirty
//     idle rung under those conditions has nowhere left to hide.
//   - Rung stats are padded and aligned so no rung pays a straddled cache
//     line its neighbours do not (see Gpu64LadderStats).
//   - `L=` is now reported as a pair, so this round is comparable with rounds
//     4-6 regardless of the threshold change made in round 7.
//
// Spacing is tight from 16 K to 512 K because that is where the answer has to
// be, and loose above it because above it the answer is already known.
//
// `NT` and `max` are gone. Non-temporal stores were ruled out in round 6, and
// `max` existed to guarantee something failed -- the 2 M rung does that.
#define GPU64_LADDER_RUNGS			13

// Working-set sizes. Core 1 always writes into the same static buffer; a rung
// uses only the first this-many bytes of it, so rungs differ in footprint and
// in nothing else -- same code, same alignment, same pages. The shared L2 on
// this part is 512 KB and the L1 D-cache is 32 KB.
#define GPU64_LADDER_SET_16K		( 16 * 1024 )
#define GPU64_LADDER_SET_64K		( 64 * 1024 )
#define GPU64_LADDER_SET_128K		( 128 * 1024 )
#define GPU64_LADDER_SET_192K		( 192 * 1024 )
#define GPU64_LADDER_SET_256K		( 256 * 1024 )
#define GPU64_LADDER_SET_320K		( 320 * 1024 )
#define GPU64_LADDER_SET_384K		( 384 * 1024 )
#define GPU64_LADDER_SET_448K		( 448 * 1024 )
#define GPU64_LADDER_SET_512K		( 512 * 1024 )
#define GPU64_LADDER_SET_768K		( 768 * 1024 )
#define GPU64_LADDER_SET_1M			( 1024 * 1024 )
#define GPU64_LADDER_SET_DRAM		( 2 * 1024 * 1024 )

// The one rate every footprint rung runs at. Round 5 showed rate is not a
// variable, so holding it fixed keeps footprint the only thing moving.
#define GPU64_LADDER_SWEEP_RATE		( 64 * 1000 )

// Wall-clock window per rung, microseconds. 2s x 13 rungs = 26s of ladder;
// the test PRG runs longer than that on purpose (see
// Source/TestPRG/gpu64_ladder_test.a).
#define GPU64_LADDER_RUNG_US		2000000

// The buffer core 1 writes into, sized for the largest arm. Rungs take a
// prefix of it (GPU64_LADDER_SET_*), so the whole ladder shares one
// allocation and one alignment.
#define GPU64_LADDER_BUFFER_BYTES	GPU64_LADDER_SET_DRAM

// Core 0 re-reads core 1's published rung index every this many passes,
// rather than every pass. This is the *only* time core 0 touches a line
// another core writes, so the interval is exactly the exposure: at 16384
// passes it is about 17ms, i.e. ~60 opportunities per second against rung
// boundaries 2 seconds apart. Granularity costs nothing here and exposure is
// the thing that has been ruining runs.
#define GPU64_LADDER_RUNG_POLL		16384

// Samples core 0 spends measuring the undisturbed loop before it fixes the
// missed-cycle threshold. ~100ms at rung 0 (idle), i.e. the first 2.5% of
// rung 0 is calibration and is not counted in that rung's L=.
//
// The threshold comes off that window's **mean**, not its minimum. Round 3
// (2026-08-23) was a visibly perfect run that the table scored as
// 31,973,007 missed cycles out of 32,140,000 passes, because the minimum is
// not "one C64 cycle" -- it is the *fastest* pass. The loop is PHI-locked
// but the lock has slop both ways: when the previous pass overran into the
// next VIC half-cycle, this pass's WAIT_FOR_VIC_HALFCYCLE returns
// immediately and the delta comes out short. Round 3 measured min 865 /
// max 1716 against a nominal 1421 (a 1.0150us PAL cycle at the 1400MHz
// config.txt pins with force_turbo), so 1.5x the minimum sat *below* the
// typical healthy pass and tripped on nearly all of them. The mean is the
// estimator that actually means "one C64 cycle", and it is sound as long as
// misses are rare in the window it is taken over -- which is what putting
// the calibration window at rung 0 is for.
//
// The multiplier is 4/3, not 3/2, and that also cost a round. Loop passes
// quantise to VIC *half*-cycles, so the delta ladder is 1420 (healthy), then
// ~2130 for one missed half-cycle, then ~2840 for a whole missed cycle. A
// 3/2 threshold is 2130 -- sitting exactly on the first failure step, which
// it then fails to exceed. Round 6's first NT rung scored `L=0 x=2101`: a
// missed half-cycle, recorded as clean. 4/3 gives 1893, comfortably above
// the highest healthy pass ever measured (1710, across five rounds) and
// comfortably below that step. Rounds 4 and 5 stay comparable either way,
// since their clean rungs all topped out at 1705 and their failing rungs at
// 4400+.
//
// Both counts are reported, as `L=lost/lost2`. The second is passes past
// **2x** the baseline -- a whole missed C64 cycle rather than a half. Having
// the pair means a round stays comparable with every earlier one whatever the
// first multiplier is set to, and it separates "core 0 arrived a phase late"
// from "core 0 missed a bus cycle outright", which are different severities
// and were previously lumped together.
#define GPU64_LADDER_CALIB_PASSES	100000

// reuUsingPolling()'s instruction-cache preload window, and the size the
// callers preload the whole function at. The ladder's per-pass measurement
// adds ~500 bytes of hot loop code, which would otherwise fall outside the
// stock 0x1a00 window and leave code that runs *every pass* unpreloaded --
// timing jitter attributed to core 1 that was really the instrument's own.
// Widened only in the ladder build; a Cortex-A53's 32KB L1 I-cache has room
// to spare, and the rolling 64-bytes-per-pass preload just takes
// proportionally longer to walk the window.
//
// Keep this at or above the function's actual size, and check after any
// change to the instrumentation -- 0x1e00 was 68 bytes clear of it by round
// 5, which is not a margin:
//   aarch64-none-elf-nm -S external/Circle/app/Firmware/kernel8.elf \
//     | grep reuUsingPolling
#ifdef GPU64_LADDER_ENABLED
	#define GPU64_POLL_IPL_WINDOW	0x2000
	#define GPU64_POLL_PRELOAD_SIZE	( 1024 * 8 )
#else
	#define GPU64_POLL_IPL_WINDOW	0x1a00
	#define GPU64_POLL_PRELOAD_SIZE	( 1024 * 7 )
#endif

// Core 0's per-pass bookkeeping -- and **everything core 0 touches on the hot
// path**, deliberately gathered into one 64-byte line that no other core ever
// writes.
//
// This is the fix for round 8, and the bug it fixes had been in the ladder
// since round 1. The state used to be loose globals, and the linker put
// `gpu64LadderThreshold` (read by core 0 on every pass) at 0x690c364 and
// `gpu64LadderRung` (written by core 1 on every millisecond slot) at
// 0x690c370 -- **the same cache line**. So core 1 invalidated core 0's
// hottest line a thousand times a second, in every rung including `idle`,
// where core 1 writes no data at all.
//
// That explains the whole shape of rounds 7 and 8: a dirty idle rung with
// core 1 asleep; 16 KB scoring 128 missed cycles in round 8 after scoring
// zero at 450 MB/s in round 5; the numbers shifting when unrelated code
// changed; and the improvement from 30 to 15 when round 8 padded the stats
// array, which moved these addresses without meaning to. **Rounds 4, 5 and 6
// were clean by link-layout luck, not by design.**
//
// A coherence miss is only ~100 cycles, but the loop is PHI-locked: it must
// finish before the next VIC half-cycle, and when it does not it waits out a
// whole one. So a 100-cycle hiccup quantises into a 710-cycle miss, which is
// exactly the 2130-cycle passes the table has been reporting.
//
// It is also a lesson for milestone 6 proper, not just for the instrument:
// any variable the render core writes that shares a line with something core
// 0 reads per pass will do this, and it will look like a mysterious,
// build-dependent timing fault.
//
// The struct is deliberately a struct rather than the four loop locals it
// started as: as locals they added register pressure to a loop already at the
// limit, and the compiler paid for them by spilling other hot values -- ~500
// bytes of extra code for a ten-instruction measurement.
struct Gpu64LadderPass
{
	u64	prev;
	u32	skip;
	u32	tick;
	u32	rung;			// core 0's own copy, refreshed rarely from core 1's
	u32	threshold;		// ARM cycles; 0 until calibrated
	u32	threshold2;		// 2x baseline: a whole missed C64 cycle
	u32	baseline;		// ARM cycles in one C64 cycle: the mean
	u32	dumped;			// so the table prints once, not twice
	u32	pad[ 7 ];
} __attribute__(( aligned( 64 ) ));

struct Gpu64LadderStats
{
	u32	passes;
	u32	lost;			// passes past the 4/3 threshold: a missed half-cycle or worse
	u32	lost2;			// passes past 2x: a whole missed C64 cycle or worse
	u32	minDelta;		// ARM cycles between consecutive loop passes
	u32	maxDelta;
	u32	pad;
	u64	totalDelta;
};

#ifdef GPU64_LADDER_ENABLED

// The two cross-core mailboxes, each alone on its own cache line so that
// neither can invalidate anything the other core reads on a hot path. See the
// note on Gpu64LadderPass for what happens when they are not.

// Written by core 1 once per millisecond slot; read by core 0 once every
// GPU64_LADDER_RUNG_POLL passes.
struct Gpu64LadderFromWorker
{
	volatile u32 rung;
	u32 pad[ 15 ];
} __attribute__(( aligned( 64 ) ));

extern Gpu64LadderFromWorker gpu64LadderFromWorker;

// Written by core 0 only when the ladder is armed; read by core 1 once per
// slot. `gen` is bumped on every arming and is what makes core 1 restart its
// own rung timeline -- without it core 1 latched its start time exactly once,
// and a second arming (which any C64 reset causes, since resetREU() clears
// gpu64ApiActive and the next GET_INFO re-arms) left core 1 running the first
// run's clock. Round 6 hit that: after a transient derail the ladder
// re-armed, core 1's clock was already past the last rung, and it published
// `done` for the remaining 51 seconds.
struct Gpu64LadderToWorker
{
	volatile u32 gen;
	volatile u32 start;
	u32 pad[ 14 ];
} __attribute__(( aligned( 64 ) ));

extern Gpu64LadderToWorker gpu64LadderToWorker;

// Index GPU64_LADDER_RUNGS is the "ladder finished, core 1 idle again"
// bucket -- core 1 stops generating traffic there, so the machine is quiet
// while the log is read.
extern Gpu64LadderStats gpu64Ladder[ GPU64_LADDER_RUNGS + 1 ] __attribute__(( aligned( 64 ) ));
extern Gpu64LadderPass gpu64LadderPass;

// Core 1's entry point, called from CGpu64MultiCore::Run().
void gpu64_ladderWorker( void );

// Called from core 0 on that transition: clears the table and releases
// core 1.
void gpu64_ladderArm( void );

// Formats the table for the on-screen log. Returns the end pointer.
char *gpu64_ladderReport( char *p );

// Forces the log on and prints the table (gpu64_api.cpp). Declared here as
// well as in gpu64_api.h because GPU64_LADDER_SAMPLE now calls it, and
// rad_reu.cpp includes this header, not that one.
void gpu64_ladderDumpNow( void );

// --- core 0's per-pass instrumentation ---------------------------------
//
// Declared as macros rather than a function because it has to live inside
// reuUsingPolling()'s loop and keep its state in registers; a call would
// cost more than the thing it measures.
//
// GPU64_LADDER_SAMPLE goes at the *top* of the loop, right after the pass's
// WAIT_FOR_VIC_HALFCYCLE, and reads PMCCNTR_EL0 itself rather than reusing
// the loop's armCycleCounter. The tempting spot -- the mid-loop
// RESTART_CYCLE_COUNTER, where a fresh value is already in hand -- sits
// immediately before WAIT_UP_TO_CYCLE( WAIT_FOR_SIGNALS ), a hard deadline
// measured in nanoseconds; inserting anything there would push the bus
// sample late and the instrument would be creating the fault it is looking
// for. The top of the loop is the slack region the mirror snapshot already
// lives in, and it is PHI-locked just the same, so the delta means the same
// thing.

#define GPU64_LADDER_LOCALS		/* state lives in gpu64LadderPass */

// The loop is PHI-locked: every pass syncs to the same edge, so consecutive
// samples are one C64 cycle apart unless a cycle was missed, in which case
// the delta jumps to a multiple of it. That is what makes a single
// subtraction a *missed bus sample* counter rather than a vague jitter
// metric.
#define GPU64_LADDER_SAMPLE { \
	u64 ladderNow; \
	asm volatile( "MRS %0, PMCCNTR_EL0" : "=r" (ladderNow) ); \
	u32 d = (u32)( ladderNow - gpu64LadderPass.prev ); \
	gpu64LadderPass.prev = ladderNow; \
	if ( gpu64LadderPass.skip ) \
		gpu64LadderPass.skip = 0; \
	else { \
		Gpu64LadderStats *s = &gpu64Ladder[ gpu64LadderPass.rung ]; \
		s->passes++; \
		s->totalDelta += d; \
		if ( d > s->maxDelta ) s->maxDelta = d; \
		if ( d < s->minDelta ) s->minDelta = d; \
		if ( gpu64LadderPass.threshold ) { \
			if ( d > gpu64LadderPass.threshold ) { \
				s->lost++; \
				if ( d > gpu64LadderPass.threshold2 ) s->lost2++; \
			} \
		} else \
		if ( s->passes >= GPU64_LADDER_CALIB_PASSES ) { \
			gpu64LadderPass.baseline = (u32)( s->totalDelta / s->passes ); \
			gpu64LadderPass.threshold2 = gpu64LadderPass.baseline * 2; \
			gpu64LadderPass.threshold = gpu64LadderPass.baseline + gpu64LadderPass.baseline / 3; \
		} \
	} \
	if ( ++gpu64LadderPass.tick >= GPU64_LADDER_RUNG_POLL ) { \
		gpu64LadderPass.tick = 0; \
		gpu64LadderPass.rung = gpu64LadderFromWorker.rung; \
		if ( gpu64LadderPass.rung >= GPU64_LADDER_RUNGS ) GPU64_LADDER_DUMP \
	} }

// A DMA hold (REU transfer, gpu64 command, mirror snapshot, deferred flip
// commit) deliberately stops the loop sampling the bus, so the pass it
// lands in is not a missed cycle and must not be counted as one. Every
// hold site raises this; the next sample is discarded.
#define GPU64_LADDER_SKIP	gpu64LadderPass.skip = 1;

// One-shot, and reached three ways: the two crash/exit paths, and -- since
// round 5 -- core 0 itself, the moment the published rung index reaches the
// `done` bucket. That last one is why the ladder no longer depends on the C64
// surviving to the end of the run: round 4 lost seven rungs of data because a
// corrupted gpu64 command dropped the machine into the RAD menu 1.2s into
// rung 1, and a table nobody can read is the same as no measurement.
//
// It stalls the loop for as long as it takes to render ~15 log lines, which
// is many C64 cycles. That is deliberate and safe *here*: it fires only once
// the ladder is over, with core 1 back at idle, and the test PRG is by then
// doing nothing a stalled bus can corrupt. The pass it lands in is discarded
// via the skip flag on the way out.
#define GPU64_LADDER_DUMP { \
	if ( !gpu64LadderPass.dumped ) \
		{ gpu64LadderPass.dumped = 1; gpu64_ladderDumpNow(); GPU64_LADDER_SKIP } }

#else

#define GPU64_LADDER_LOCALS
#define GPU64_LADDER_SAMPLE
#define GPU64_LADDER_SKIP
#define GPU64_LADDER_DUMP

#endif

#endif
