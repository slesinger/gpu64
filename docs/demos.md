# gpu64 demonstration programs

Nine C64 programs in [`Source/Demos/`](../Source/Demos/), each one showing a
part of the API doing something you can look at. They are written to be
read: heavily commented, no shared cleverness beyond an include of the
register equates, and none of them depends on another.

Hardware status, which differs per program — see
[progress_tracker.md](progress_tracker.md) for the rounds themselves:

- the first six were verified on 2026-08-25;
- `raycast` ran in the round that closed tracker section 8b, and the blink
  it was named for did not reproduce;
- `sectors` was verified on 2026-08-25, in the round that added
  `DRAW_THINGS`;
- `walls` has been rendered on a PC and not separately checked at the bench.

This is not the conformance suite. The suite
([`Source/TestPRG/`](../Source/TestPRG/), run by `tools/testprg.sh`) asserts
that the firmware matches [api_design.md](api_design.md) and prints a
verdict. The demos assert nothing — they are what you copy from.

## The nine

| Demo | Shows | Commands |
|---|---|---|
| `hello` | The first program to run. One picture, one line of result on the C64 screen. Clipping is normal, not an error. | `SET_BORDER`, `CLEAR`, `RECT_FILL`, `RECT`, `LINE`, `SET_PIXEL` |
| `palette` | 256 colours, and animation without touching the framebuffer: a hundred rings drawn *once*, then cycled with one command per frame. | `PAL_LOAD`, `PAL_SET`, `RECT` |
| `sprites` | The three bulk-pixel commands side by side — the opaque blit's box, the keyed blit without one, and the save/restore idiom that moves a sprite over a background on a single page. | `BLIT`, `BLIT_KEYED`, `READ_RECT` |
| `bounce` | The tear-free animation skeleton, and the loop worth copying verbatim: draw into the page nobody can see, flip on a frame boundary. | `SET_DRAW_PAGE`, `PAGE_FLIP`, `VBLANK_ACK`, `VBLANK_SYNC` |
| `rotate` | The matrix ops driving graphics. A shape scaled and rotated by gpu64 every frame; the 6502 does no multiplication at all. | `MAT_SCALE`, `MAT_MUL` |
| `matrix` | The math API as a coprocessor, with every answer printed in hex on the C64's screen — including where 8.8 loses a bit and float32 does not, and two error paths. | `MAT_IDENTITY`, `MAT_INVERSE`, `MAT_MUL`, `FLT_INVERSE`, `FLT_MUL` |
| `raycast` | Class 2, and the reason it exists: a textured, lit, first-person view at 320x160 from a 1MHz 6502 that never touches a pixel. 480 primitives a frame in **four** dispatches; the 6502 marches 40 rays with no multiply and no divide and lets gpu64 do the clipping, the perspective `v` stepping and the lighting. Also the batch checksum used where it is free, and `RASTER_STATS` read back every frame. | `SET_VIEW`, `SET_COLORMAP`, `UPLOAD_TEXTURE`, `DRAW_COLUMNS`, `DRAW_SPANS`, `DRAW_SPRITE`, `RASTER_STATS`, `PAL_LOAD`, `RECT_FILL`, `PAGE_FLIP` |
| `walls` | The step past `raycast`: the same kind of picture with **no raycaster on the 6502 at all**. A 16x16 level is 162 wall segments in world coordinates, built by the assembler and never rewritten; a frame is ten bytes of camera and two dispatches. gpu64 does the projection, the perspective texture mapping, the distance lighting and the depth sort, and `CAM_PAINT` gets the floor and ceiling for free. The batch is checksummed on every frame because the level is static, so the sum is computed once. | `SET_CAMERA`, `DRAW_WALLS`, `SET_VIEW`, `SET_COLORMAP`, `UPLOAD_TEXTURE`, `DRAW_SPRITE`, `RASTER_STATS`, `PAL_LOAD`, `RECT_FILL`, `PAGE_FLIP` |
| `sectors` | The step past `walls`: the same level shape with **sectors**. Every open cell belongs to a sector with its own floor height, ceiling height and flat colours, so the level has a raised platform to step onto, a corridor a quarter of a unit up and only one unit tall, and a courtyard open to the sky. The wall at each end of that corridor is **two-sided** -- it draws a band above the far ceiling, a band below the far floor, and leaves a window between them. Six barrels stand in it as **things** -- billboards at world positions, depth-tested per pixel against the level, so the one in the corridor is cut off at the base by the step it stands behind and hidden entirely when the doorway is out of view. One of them paces up and down the courtyard, which is why that batch carries a checksum recomputed every frame while the level's is computed once. The 6502 adds one map lookup for the eye height of the sector it is standing in; everything else is what `walls` costs. | `SET_SECTORS`, `DRAW_SECTORS`, `DRAW_THINGS`, `SET_CAMERA`, `SET_VIEW`, `SET_COLORMAP`, `UPLOAD_TEXTURE`, `FILL_VIEW`, `DRAW_SPRITE`, `RASTER_STATS`, `PAL_LOAD`, `RECT_FILL`, `PAGE_FLIP` |

Every one of them ends on RUN/STOP.

## Building and checking them

```
tools/demos.sh              # assemble, run on the PC, render all nine
tools/demos.sh -v rotate    # just this one, and print the C64 screen
```

Each demo is run twice under [`tools/prgsim`](../tools/prgsim/): once
modelling a display gpu64 measured a frame period from, and once modelling
one it could not, where every vblank feature answers `UNSUPPORTED`. A demo
has to survive both, which is why the runtime's `dmVbWait` / `dmFlip` fall
back to free-running and immediate flips instead of refusing to run.

What the run leaves in `Source/Demos/out/<name>.ppm` is what the HDMI output
was showing when the program returned — the visible page, through the
current palette, inside the border. **Look at those before deploying to
hardware.** Bench time is the scarce resource, and a demo that renders a
black rectangle on a PC will render a black rectangle on the bench.

## Running them on the C64

Copy the `.prg` files onto whatever the machine loads from, then

```
LOAD"GPU64-HELLO",8,1
RUN
```

In the RAD menu, press **T until it reads REU**. Everything gpu64 does
lives inside the REU polling loop; without that, nothing runs at all and the
HDMI screen keeps showing the text mirror.

## The two include files

- `gpu64_demo.inc` — the register window, the opcode numbers, the error
  codes, four macros for staging an argument block (`#argb`, `#argw`,
  `#argblob`, `#argcd`) plus `#cmd` to fire one, and — for class 2 — three
  more for staging batch records (`#r2col`, `#r2span`, `#r2batch`) and
  `#cmd2` / `#cmd0` to switch class and fire in one go. Copy this into your own
  project; it is the part of a gpu64 program that is the same in every
  gpu64 program.
- `gpu64_demo_rt.inc` — housekeeping the demos share and the API knows
  nothing about: text and hex on the C64 screen, the STOP key, and the two
  vblank helpers with their no-frame-clock fallback. Included **last**,
  because it is code and data rather than macros.

A demo's skeleton is then:

```asm
	.include "gpu64_demo.inc"
	#basicStub			; "10 SYS 2064", entry at $0810

start
	jsr dmInit			; clear the screen, class 0, read GET_INFO
	#argb 0, BLUE
	#cmd OP_CLEAR
	jmp dmHold
	.include "gpu64_demo_rt.inc"
```
