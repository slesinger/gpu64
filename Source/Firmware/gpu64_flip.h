/*
 gpu64: the page flip, without the mailbox round trip.

 The problem this solves: milestone 4b's deferred PAGE_FLIP commits from
 inside the bus-watch loop's DMA hold, and the only thing it does there is
 CBcmFrameBuffer::SetVirtualOffset() -- a *synchronous* property-mailbox
 call to the VideoCore. Measured on hardware over 256 flips: 71 us at best,
 ~900 us typically. The C64 is halted for all of it, so a program flipping
 every frame hands roughly 5% of its cycles to the commit, and a hold that
 long can span a raster and flash the VIC-II.

 Almost none of that cost is the ARM's. Writing the request into MAILBOX1 is
 a status poll and one 32-bit store; the rest is waiting for the VideoCore's
 mailbox task to be *scheduled*, which is also why the figure is so erratic.
 So this module splits the round trip in half:

   gpu64_flipPost()   -- inside the DMA hold: store the offset, write the
                         mailbox, return. Sub-microsecond, and it does not
                         care when the VideoCore gets around to it.
   gpu64_flipDrain()  -- outside the hold, at the top of the next command
                         dispatch: consume the reply.

 The drain is not just bookkeeping, it is what keeps the split safe. Once
 posted, the flip is in flight and the page the C64 may now draw into is not
 provably off-screen yet. But the C64 cannot draw anything without issuing a
 command, and every command drains first -- so by the time any drawing can
 happen, the VideoCore has processed the flip. The race is closed by
 construction rather than by timing luck.

 Draining also matters for a duller reason: Circle's CBcmMailBox::WriteRead()
 begins with a Flush() that discards stale replies with a *20 ms* delay
 each. An unconsumed reply of ours sitting in MAILBOX0 when Circle next uses
 the mailbox (PAL_SET, VBLANK_SYNC) would cost exactly that. Hence a drain
 before every such path, not only before the next post.

 Everything here is a deliberate duplicate of what CBcmPropertyTags does,
 for the one tag that needs to be cheap. It is not a general mailbox client
 and should not grow into one.
*/
#ifndef _gpu64_flip_h
#define _gpu64_flip_h

#include <circle/bcm2835.h>
#include <circle/memio.h>
#include <circle/types.h>

// How long gpu64_flipDrain() waits for a reply before giving up, in
// microseconds. Generous by two orders of magnitude against the ~900 us the
// round trip actually takes -- this is a "the VideoCore is not answering"
// bound, not a performance one. Hitting it disarms the fast path for the
// rest of the session and falls back to Circle's blocking call, which is
// slow but known to work.
#define GPU64_FLIP_DRAIN_TIMEOUT_US	50000

// Per-flip cost, kept because the whole point of this module is a number
// that was measured and has to be re-measured to know it moved. Reported by
// LOG_ENABLE(1), which is exempt from the log auto-hide and so is the one
// command a bench program can use to read firmware state back.
struct GPU64FLIPSTATS
{
	u32	postCount;	// flips posted through the fast path
	u32	postMinUs;	// time spent inside the DMA hold
	u32	postMaxUs;
	u32	postTotalUs;

	u32	drainCount;	// drains that actually had a reply to wait for
	u32	drainMinUs;	// time the *next command* paid to finish the flip
	u32	drainMaxUs;
	u32	drainTotalUs;

	u32	slowCount;	// flips that fell back to the blocking mailbox
};

extern GPU64FLIPSTATS gpu64FlipStats;

// Builds the property buffer and arms the fast path. Call once, after the
// framebuffer is initialized (it needs nothing from it, but a fast flip
// before there is anything to flip is meaningless). Returns FALSE if the
// coherent page is unavailable, in which case every flip stays on Circle's
// blocking call and nothing else changes.
boolean gpu64_flipInit( void );

// TRUE if gpu64_flipPost() may be used. FALSE after a failed init or a
// drain timeout.
boolean gpu64_flipAvailable( void );

// Posts SET_VIRTUAL_OFFSET(0, nOffsetY) without waiting for the reply.
// Safe to call from inside the DMA hold. Returns FALSE, having done
// nothing, if the fast path is disarmed or a flip is still in flight --
// the caller has to drain first, and cannot do it from the hold.
boolean gpu64_flipPost( u32 nOffsetY );

// Preloads gpu64_flipPost() into the instruction cache. Called from the
// PAGE_FLIP dispatch, which already holds the bus -- never from the loop.
void gpu64_flipWarm( void );

// Waits for an in-flight flip's reply, if any. No-op when nothing is in
// flight, which is the common case at a command dispatch. Never call this
// from the bus-watch loop.
void gpu64_flipDrain( void );

#endif
