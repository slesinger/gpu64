/*
 gpu64: frame clock calibration. See gpu64_vsync.h for why this exists and
 why the bus-watch loop never calls any of it.
*/
#include "gpu64_vsync.h"
#include "gpu64_fb.h"

GPU64VSYNC gpu64Vsync = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };

boolean gpu64_vsyncCalibrate( void )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	if ( pFB == 0 || !pFB->IsInitialized() )
		return FALSE;

	// Start on a real boundary, so the span below covers whole frames and
	// not a partial one plus the latency of getting here.
	if ( !pFB->WaitForVSync() )
		return FALSE;
	u32 t0 = gpu64_vsyncNow();

	for ( unsigned i = 0; i < GPU64_VSYNC_CAL_FRAMES; i++ )
		if ( !pFB->WaitForVSync() )
			return FALSE;
	u32 t1 = gpu64_vsyncNow();

	u32 period = ( t1 - t0 ) / GPU64_VSYNC_CAL_FRAMES;
	if ( period < GPU64_VSYNC_MIN_PERIOD || period > GPU64_VSYNC_MAX_PERIOD )
		return FALSE;

	gpu64Vsync.periodUs   = period;
	gpu64Vsync.nextUs     = t1 + period;
	gpu64Vsync.frameCount = 0;
	gpu64Vsync.calibrated = 1;
	return TRUE;
}

boolean gpu64_vsyncReanchor( void )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	if ( pFB == 0 || !pFB->IsInitialized() || !gpu64Vsync.calibrated )
		return FALSE;

	if ( !pFB->WaitForVSync() )
		return FALSE;

	gpu64Vsync.nextUs = gpu64_vsyncNow() + gpu64Vsync.periodUs;
	return TRUE;
}

void gpu64_vsyncResetState( void )
{
	gpu64Vsync.armed         = 0;
	gpu64Vsync.flipPending   = 0;
	gpu64Vsync.irqRequest    = 0;
	gpu64Vsync.irqReleaseReq = 0;
	// irqAsserted is deliberately left alone: it describes the state of a
	// physical pin, and only the bus-watch loop may change that. If the line
	// is still held, the loop's release path clears it.
	if ( gpu64Vsync.irqAsserted )
		gpu64Vsync.irqReleaseReq = 1;

	// A fresh session starts its frame numbering at zero, but the clock
	// itself keeps running -- the display did not stop between sessions.
	gpu64Vsync.frameCount = 0;
}
