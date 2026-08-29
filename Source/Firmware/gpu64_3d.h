/*

  {_______            {_          {______
        {__          {_ __               {__
        {__         {_  {__               {__
     {__           {__   {__               {__
 {______          {__     {__              {__
       {__       {__       {__            {__
         {_________         {______________		Expansion Unit

 gpu64 milestone 6 -- the class 1 (3D) subsystem, public interface.

 Wire protocol: project/milestone6_3d_design.md. That document is the spec;
 this header is the firmware side of it and deliberately does not restate
 the rationale.

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

*/
#ifndef _gpu64_3d_h
#define _gpu64_3d_h

#include <circle/types.h>

// gpu64: the whole class 1 subsystem behind one toggle, the same way the
// ladder and the multicore spike are. Off = gpu64_apiDispatch() keeps
// answering class 1 with BAD_CLASS exactly as it does today, core 1 stays
// parked, and nothing below is linked in.
//
// ON by default on this branch, and that is a real change to the shipping
// configuration: it brings CMultiCoreSupport::Initialize() up in every
// build, which milestone 6a showed is safe on its own but which the
// milestone 4 releases did not do. Comment it out to get the proven
// single-core build back -- that is the bisect handle if anything in
// class 0 regresses.
#define GPU64_3D_ENABLED

// --- class 1 opcodes ----------------------------------------------------
// Full table with argument layouts: project/milestone6_3d_design.md. Only the
// ones a given phase implements are dispatched; the rest are BAD_OPCODE
// until they exist, which is what keeps a half-built class honest.

// System and loop -- $00-$0F
#define GPU64_3D_OP_SCENE_RESET		0x00
#define GPU64_3D_OP_SET_VIEWPORT	0x01
#define GPU64_3D_OP_SET_PERSPECTIVE	0x02
#define GPU64_3D_OP_SET_LIGHT		0x03
#define GPU64_3D_OP_BUILD_COLORMAP	0x04
#define GPU64_3D_OP_SET_BACKGROUND	0x05
#define GPU64_3D_OP_LOOP_START		0x06
#define GPU64_3D_OP_LOOP_STOP		0x07
#define GPU64_3D_OP_SCENE_COMMIT	0x08
// gpu64: not in the design doc's original table -- added in phase 1 because
// "you get OUT_OF_MEMORY eventually" is truthful and undebuggable against a
// 32 MB arena. RESULT is the free arena in 128 KB units, which is exactly
// 0..256 for a 32 MB arena and so fits the one byte RESULT has.
#define GPU64_3D_OP_ARENA_STATUS	0x09

// Resources -- $10-$1F
#define GPU64_3D_OP_UPLOAD_MESH		0x10
#define GPU64_3D_OP_UPLOAD_TEXTURE	0x11
#define GPU64_3D_OP_FREE_RESOURCE	0x12

// Scene nodes -- $20-$2F
#define GPU64_3D_OP_CREATE_OBJECT	0x20
#define GPU64_3D_OP_CREATE_CAMERA	0x21
#define GPU64_3D_OP_DESTROY_NODE	0x22
#define GPU64_3D_OP_SET_ACTIVE_CAMERA	0x23
#define GPU64_3D_OP_SET_VISIBLE		0x24

// Transforms -- $30-$3F
#define GPU64_3D_OP_SET_POSITION	0x30
#define GPU64_3D_OP_SET_ORIENTATION	0x31
#define GPU64_3D_OP_MOVE_LOCAL		0x32
#define GPU64_3D_OP_MOVE_WORLD		0x33
#define GPU64_3D_OP_ROTATE_LOCAL	0x34
#define GPU64_3D_OP_SET_SCALE		0x35
#define GPU64_3D_OP_GET_TRANSFORM	0x36

// Immediate mode -- $40-$4F
#define GPU64_3D_OP_CLEAR_VIEWPORT	0x40
#define GPU64_3D_OP_DRAW_MESH		0x41
#define GPU64_3D_OP_DRAW_NODE		0x42

#ifdef GPU64_3D_ENABLED

// --- core 0 entry points ------------------------------------------------

// Brings the subsystem to its power-on state: ring buffer emptied, core 1
// released. Called once at boot, before core 1 is started.
void gpu64_3dInit( void );

// Session reset -- called from gpu64_apiReset(), i.e. on every resetREU().
// Per project/milestone6_3d_design.md's Resource lifecycle section this is
// where every resource of the session is freed, so RUN/STOP+RESTORE cannot
// leave the next program running against stale IDs.
void gpu64_3dReset( void );

// Executes one class 1 command from core 0, exactly as doSystem()/doDraw()
// do for class 0: returns a GPU64_ERR_* code, having already pushed
// whatever core 1 needs to do onto the ring.
//
// Through Stage 15a this never blocked beyond a full ring (GPU64_ERR_QUEUE_
// FULL). Stage 15b (project/gap_filling_plan.md) changes that: CLEAR_
// VIEWPORT/DRAW_MESH/DRAW_NODE now push and return without waiting for their
// own draw, but every *other* class 1 opcode drains the ring before it runs
// (it is about to mutate state a still-queued render also reads), and a
// render pushed against a full ring waits, bus held, for space rather than
// rejecting outright -- see gpu64_3dSync()'s comment for what that means for
// RESULT/ERRCODE timing on the two draws. GPU64_ERR_QUEUE_FULL survives only
// for the pathological case both of those still refuse: a non-render op
// pushed against a full ring, which the design's "a failed dispatch does
// nothing" rule still answers by rejecting rather than waiting.
u8 gpu64_3dDispatch( u8 op );

// Core 1's entry point, called from CGpu64MultiCore::Run(). Never returns.
void gpu64_3dWorker( void );

// Stage 15b's observation-point hook for gpu64_api.cpp: waits for the ring
// to fully drain, publishing the most recently queued CLEAR_VIEWPORT/
// DRAW_MESH/DRAW_NODE's RESULT to gpu64Regs once it is known complete.
// Returns FALSE only on the drain timeout backstop (a wedged core 1).
//
// Call this before anything that would otherwise race a render this file has
// queued but not yet run on core 1: a page flip/vsync commit, or a class 0
// opcode that reads or writes the framebuffer. (Every non-render class 1
// opcode already does the equivalent internally, via its own dispatch --
// callers here do not need to call this before *those*.)
//
// This is also how a program gets a DRAW_MESH/DRAW_NODE's own RESULT (the
// triangle count) back on demand: RESULT is valid as of the last observation
// point, not the instant dispatch returns, so issuing any op that reaches
// this -- including just the next DRAW_NODE -- and then reading RESULT gets
// it. See docs/class1-3d-mesh-reference.md's "Deferred RESULT" note.
boolean gpu64_3dSync( void );

// Formats the subsystem's counters for the on-screen log (bring-up aid,
// phase 0's only output). Returns the end pointer.
char *gpu64_3dReport( char *p );

#endif	// GPU64_3D_ENABLED

#endif
