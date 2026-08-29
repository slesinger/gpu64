/*
 gpu64: the IO2 command API register file and dispatcher.

 Wire protocol reference (register map, opcode table, error codes):
 docs/api_design.md. Design rationale: project/milestone4_2d_api_design.md.

 Everything here runs synchronously on core 0, called straight from
 reuUsingPolling()'s IO2 write handler in rad_reu.cpp -- the same
 immediate-mode model milestone 2's test pattern and milestone 3's mirror
 already use. Nothing here allocates, blocks, or loops unboundedly: the one
 cost that scales is a blob transfer, and it scales with a length the C64
 itself chose.
*/
#ifndef _gpu64_api_h
#define _gpu64_api_h

#include <circle/types.h>

// --- register offsets within IO2 (i.e. $DFxx) ---------------------------
#define GPU64_REG_CMD_HI	0x0B
#define GPU64_REG_CMD_LO	0x0C
#define GPU64_REG_STATUS	0x0D
#define GPU64_REG_ERRCODE	0x0E
#define GPU64_REG_ID_LO		0x0F
#define GPU64_REG_ID_HI		0x10
#define GPU64_REG_ARG0		0x11
#define GPU64_REG_ARG15		0x20
// gpu64: class 1 only. Low byte of the last command's result -- the page
// number from SCENE_COMMIT, the allocated ID from a CREATE_*. Meaning is per
// opcode and undefined for opcodes that define none.
#define GPU64_REG_RESULT	0x21

#define GPU64_ARG_COUNT		16

// --- STATUS bits --------------------------------------------------------
#define GPU64_STATUS_BUSY		0x01
#define GPU64_STATUS_ERROR		0x02
#define GPU64_STATUS_VBLANK_PENDING	0x04
#define GPU64_STATUS_VBLANK_ARMED	0x08
// gpu64: class 1. The render loop has finished a frame and is waiting for
// SCENE_COMMIT (handshake mode only).
#define GPU64_STATUS_FRAME_READY	0x10

// --- ERRCODE values -----------------------------------------------------
#define GPU64_ERR_OK			0x00
#define GPU64_ERR_BAD_OPCODE		0x01
#define GPU64_ERR_BAD_CLASS		0x02
#define GPU64_ERR_OUT_OF_RANGE		0x03
#define GPU64_ERR_BAD_ARGS		0x04
#define GPU64_ERR_SINGULAR		0x05
// gpu64: defined by the spec, not implemented by this build. As of the
// frame clock (gpu64_vsync.h) the vblank features are implemented, so this
// now only covers the case where the clock could not be calibrated at boot
// -- a display that never reports a vertical sync. Better than firing
// vblank events at a made-up rate.
#define GPU64_ERR_UNSUPPORTED		0x06
// gpu64: a deferred PAGE_FLIP is still waiting for its frame boundary. The
// spec's rule that a failed dispatch does nothing applies -- the queued flip
// is untouched. Poll STATUS bit0 before asking for another.
#define GPU64_ERR_BUSY			0x07

// gpu64: class 1 (milestone 6). See project/milestone6_3d_design.md.
// Resource RAM exhausted.
#define GPU64_ERR_OUT_OF_MEMORY		0x08
// The core-0-to-core-1 ring buffer is full. Core 0 rejects rather than waits
// -- its cycle-predictability outranks any one command's completion.
#define GPU64_ERR_QUEUE_FULL		0x09
// No such resource or scene node.
#define GPU64_ERR_BAD_ID		0x0A
// A render was asked for with no active camera.
#define GPU64_ERR_NO_CAMERA		0x0B

// gpu64: Stage 15a. gpu64_3dDispatch() pushes a validated CLEAR_VIEWPORT /
// DRAW_MESH / DRAW_NODE onto the core-0/core-1 ring and then waits for core 1
// to drain it (see gpu64_3d_class1.cpp's waitForDrain()); this is what it
// returns if the backstop timeout fires first. It is not a validation
// failure -- the command already passed precheck, the same checks that used
// to produce BAD_ID/BAD_ARGS/UNSUPPORTED for these opcodes -- it means core 1
// did not finish in a generous multiple of any real draw's worst case, i.e.
// core 1 is wedged. Designed to be unreachable in normal operation; see
// project/gap_filling_plan.md's Stage 15 section.
#define GPU64_ERR_WORKER_TIMEOUT	0x0C

// --- blob descriptor spaces --------------------------------------------
#define GPU64_SPACE_C64			0
#define GPU64_SPACE_REU			1

// --- register file ------------------------------------------------------
// gpu64: deliberately a plain global, not statics behind accessor calls.
// The C64's ARG writes and STATUS/ERRCODE reads are serviced from inside
// reuUsingPolling()'s cycle-critical window, where the handler has to be
// done before a hard deadline (WAIT_CYCLE_READ2 on the read side). A call
// into another translation unit costs an instruction-cache miss the first
// time and a branch every time; missing the deadline means the C64 latches
// garbage, or the loop is still busy when the next IO2 access arrives and
// never sees it. So those two paths are inlined below and touch this struct
// directly -- only the dispatcher, which runs with the C64 DMA-halted and
// has no deadline, stays a real call.
struct GPU64REGS
{
	u8	cmdHi;
	u8	status;
	u8	err;
	u8	id[ 2 ];
	u8	result;			// class 1, read-only from the C64
	u8	arg[ GPU64_ARG_COUNT ];
};

extern GPU64REGS gpu64Regs;

// Resets the whole register file. Called from resetREU().
void gpu64_apiReset( void );

// Executes one command. Heavy and unbounded-ish by design: the caller holds
// the bus across it (see the CMD_LO case in reuUsingPolling()).
void gpu64_apiDispatch( u8 op );

// IO2 register window write/read, minus the dispatch. addr is the full
// 8-bit IO2 offset, so the caller must NOT pass REU's 5-bit-masked version
// -- see the decode note in project/milestone4_2d_api_design.md.
static inline void gpu64_apiWriteReg( u8 addr, u8 data )
{
	if ( addr == GPU64_REG_CMD_HI )
		gpu64Regs.cmdHi = data;				// sticky, triggers nothing
	else if ( addr == GPU64_REG_ID_LO )
		gpu64Regs.id[ 0 ] = data;
	else if ( addr == GPU64_REG_ID_HI )
		gpu64Regs.id[ 1 ] = data;
	else if ( addr >= GPU64_REG_ARG0 && addr <= GPU64_REG_ARG15 )
		gpu64Regs.arg[ addr - GPU64_REG_ARG0 ] = data;
	// Everything else in $DF21-$DFFF is reserved: writes are ignored.
	// (CMD_LO never reaches here -- the caller dispatches it directly.)
}

static inline u8 gpu64_apiReadReg( u8 addr )
{
	// Bounds-checked by construction: this never indexes a struct with a
	// masked address the way REU's own read handler does (see the landmine
	// note in project/milestone4_2d_api_design.md).
	if ( addr == GPU64_REG_STATUS )
		return gpu64Regs.status;
	if ( addr == GPU64_REG_ERRCODE )
		return gpu64Regs.err;
	if ( addr == GPU64_REG_RESULT )
		return gpu64Regs.result;
	return 0xFF;
}

// --- provided by rad_reu.cpp (they need the DMA macros and REU state) ---
// Both return a GPU64_ERR_* code. A C64-space transfer costs one bounded
// DMA burst; an REU-space transfer is a plain copy out of reuMemory.
u8 gpu64_blobRead( u8 space, u32 addr, u32 len, u8 *pDst );
u8 gpu64_blobWrite( u8 space, u32 addr, u32 len, const u8 *pSrc );

// gpu64: re-warms the polling loop's instruction window and the two data
// structures it touches every pass (gpu64Regs, gpu64Vsync), both of which a
// command dispatch evicts. Must be called with the bus still held -- see the
// implementation in rad_reu.cpp for which rule this is and why.
void gpu64_apiWarmPollingLoop( void );

// gpu64: dumps the milestone 6a ladder table (and the flip stats) to the
// on-screen log immediately, from wherever the bus-watch loop is giving up.
// See gpu64_api.cpp. Compiles to an empty call in a normal build.
void gpu64_ladderDumpNow( void );

#endif
