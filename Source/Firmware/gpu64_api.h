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

// gpu64: reliability-protocol detector-only build (project/reliability_protocol_design.md,
// "Recommended next step"). SEQ is write-only, C64-chosen, cycled $01-$FE
// (never $00 or $FF); SEQACK is read-only and is set to the most recently
// *dispatched* SEQ, so a C64-side compare against the value it just wrote
// catches a dropped CMD_LO the same way a dropped ARG write would silently
// go unnoticed today. No retry logic yet -- see the design doc. Provisional
// addresses: these sit in the currently-reserved tail of the un-remapped
// $DF0B-$DF21 block, ahead of the planned UCI remap
// (project/uci_register_remap_design.md) -- acceptable here only because
// this pair is throwaway diagnostic instrumentation, not the shipped
// protocol, which will land after the remap at $DF37+.
#define GPU64_REG_SEQ		0x22
#define GPU64_REG_SEQACK	0x23

#define GPU64_ARG_COUNT		16

// --- the one-shot key for destructive opcodes ---------------------------
//
// gpu64 (2026-09-10, bench run 14). The bus loses and mis-samples writes
// (CLAUDE.md, "The bus is not reliable"), and two bench runs have now caught
// an opcode EXECUTING that the C64 never sent: run 11 a LOOP_STOP, run 14
// something that wiped the live scene's active camera. A command that only
// draws survives that -- one frame is wrong and the next is right. A command
// that destroys state does not: the session is over and nothing says why.
//
// So every destructive opcode demands this byte in ARG15, and
// gpu64_apiDispatch() SPENDS ARG15 on every dispatch of every class. The key
// is therefore valid for exactly one command, the one that staged it:
// a phantom CMD_LO write arriving at any other moment finds ARG15 zero and
// is refused with BAD_ARGS, counted in gpu64ApiDiag.keyRefused along with
// its opcode. The residue it cannot cover is a real keyed command whose own
// opcode byte is mis-sampled into a different destructive opcode, which is
// one write instead of a whole stream.
//
// $A5 rather than a small number: 10100101 shares no bit pattern with a
// plausible coordinate or id byte, and an ARG register left over from a
// previous command is overwhelmingly likely to hold one of those.
#define GPU64_KEY_DESTRUCTIVE	0xA5
#define GPU64_REG_KEY		( GPU64_ARG_COUNT - 1 )	// ARG15, i.e. $DF20

// --- the checked-command key --------------------------------------------
//
// gpu64 (2026-09-25, bench run 41). Writing every argument byte twice
// (the game's stage2) defeats a write the polling loop does not sample, but
// not one it samples with a bit flipped: the second pass is simply believed,
// and a retained scene keeps believing it. Run 41 still flickered E1M1's
// far side through the walls, and runsim --bus-fault=data showed every event
// was one of those -- a camera 4, 8 or 128 units off for one frame, a door
// displaced until the refresh ring got to it.
//
// So a command may carry its own check. ARG15 = GPU64_KEY_CHECKED | n says
// ARG14 holds the XOR of that ARG15 byte, CMD_HI, CMD_LO, ID_LO, ID_HI and
// ARG0..ARG(n-1). The length travels in the key because ARGs the command
// does not write still hold the previous command's bytes, which the C64
// cannot vouch for. A mismatch is refused with BAD_ARGS before any class
// sees the command -- nothing executes -- and an absolute command can simply
// be sent again. CMD_LO is in the sum, so a CMD_LO flipped into another
// opcode is refused too.
//
// It rides the same one-shot ARG15 as the destructive key and is spent with
// it, so it is optional and costs nothing when absent: a firmware that
// predates it sees neither $A5 nor anything else it knows, and runs the
// command unchecked. What it cannot cover is the key byte itself being lost
// or flipped out of the $D0 range -- the command then runs unchecked, which
// is where every command was before.
#define GPU64_KEY_CHECKED	0xD0	// high nibble; low nibble = n, 0..14
#define GPU64_REG_CHECK		( GPU64_ARG_COUNT - 2 )	// ARG14, i.e. $DF1F

// The value ARG15 held when the current dispatch began, before
// gpu64_apiDispatch() cleared it. Every class reads the key through this and
// never through gpu64Regs.arg[15], which by then is already zero.
extern u8 gpu64ApiKey;

// TRUE if the command now dispatching carried the key. The one test every
// destructive opcode makes.
static inline boolean gpu64_apiKeyed( void )
{
	return gpu64ApiKey == GPU64_KEY_DESTRUCTIVE;
}

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
	u8	seq;			// last SEQ value written (detector-only)
	u8	seqAck;			// SEQ of the last command actually dispatched
};

extern GPU64REGS gpu64Regs;

// Resets the whole register file. Called from resetREU().
void gpu64_apiReset( void );

// gpu64: the display half of a session reset -- mode, palette, border, pages
// and the text planes back to their post-boot state, plus the health sticky
// flags. Separate from gpu64_apiReset() because it must not touch the
// register file: FULL_RESET ($0B) calls it from inside a dispatch. See its
// definition in gpu64_api.cpp.
void gpu64_apiFullReset( void );

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
	else if ( addr == GPU64_REG_SEQ )
		gpu64Regs.seq = data;
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
	if ( addr == GPU64_REG_SEQACK )
		return gpu64Regs.seqAck;
	return 0xFF;
}

// gpu64: the window C64-space writebacks are confined to.
//
// Every GET_* opcode takes the destination address in its ARG block, and no
// ARG byte is covered by SEQ/SEQACK -- so one lost store turns "write 128
// bytes into my buffer" into "write 128 bytes over whatever address the
// previous command left in those registers", with the Pi doing the writing
// and the C64 halted while it happens. It cost a simulated run its own code
// at a fault-injected rate, which is exactly how it would present on the
// bench: a program that dies for no visible reason some time after a
// readback.
//
// So gpu64_blobWrite() refuses a C64-space destination outside this window.
// Length 0 means "the whole address space", which is the reset default and
// what every program written before $0C sees, so this changes nothing for
// anyone who does not opt in. SET_DMA_WINDOW ($0C) sets it and echoes a
// checksum in RESULT, because RESULT is readable and ARG is not.
extern u16 gpu64DmaWinBase;
extern u16 gpu64DmaWinLen;			// 0 = unrestricted

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
