# gpu64 demonstration programs

Six C64 programs in [`Source/Demos/`](../Source/Demos/), each one showing a
part of the API doing something you can look at. They are written to be
read: heavily commented, no shared cleverness beyond an include of the
register equates, and none of them depends on another.

All six were verified on hardware on 2026-08-25.

This is not the conformance suite. The suite
([`Source/TestPRG/`](../Source/TestPRG/), run by `tools/testprg.sh`) asserts
that the firmware matches [api_design.md](api_design.md) and prints a
verdict. The demos assert nothing — they are what you copy from.

## The six

| Demo | Shows | Commands |
|---|---|---|
| `hello` | The first program to run. One picture, one line of result on the C64 screen. Clipping is normal, not an error. | `SET_BORDER`, `CLEAR`, `RECT_FILL`, `RECT`, `LINE`, `SET_PIXEL` |
| `palette` | 256 colours, and animation without touching the framebuffer: a hundred rings drawn *once*, then cycled with one command per frame. | `PAL_LOAD`, `PAL_SET`, `RECT` |
| `sprites` | The three bulk-pixel commands side by side — the opaque blit's box, the keyed blit without one, and the save/restore idiom that moves a sprite over a background on a single page. | `BLIT`, `BLIT_KEYED`, `READ_RECT` |
| `bounce` | The tear-free animation skeleton, and the loop worth copying verbatim: draw into the page nobody can see, flip on a frame boundary. | `SET_DRAW_PAGE`, `PAGE_FLIP`, `VBLANK_ACK`, `VBLANK_SYNC` |
| `rotate` | The matrix ops driving graphics. A shape scaled and rotated by gpu64 every frame; the 6502 does no multiplication at all. | `MAT_SCALE`, `MAT_MUL` |
| `matrix` | The math API as a coprocessor, with every answer printed in hex on the C64's screen — including where 8.8 loses a bit and float32 does not, and two error paths. | `MAT_IDENTITY`, `MAT_INVERSE`, `MAT_MUL`, `FLT_INVERSE`, `FLT_MUL` |

Every one of them ends on RUN/STOP.

## Building and checking them

```
tools/demos.sh              # assemble, run on the PC, render all six
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
  codes, and four macros for staging an argument block (`#argb`, `#argw`,
  `#argblob`, `#argcd`) plus `#cmd` to fire one. Copy this into your own
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
