# Getting started

gpu64 gives a Commodore 64 a second, independent HDMI screen and a command
API to draw on it. This page is everything you need to issue your first
command and read your first demo; the opcode tables live in their own
per-class reference files — see [README.md](README.md) for the map.

## Register map

gpu64 owns `$DF0B`–`$DFFF` in IO2 (REU keeps `$DF00`–`$DF0A`; IO1,
`$DE00`–`$DEFF`, is untouched).

| Register | Address | Purpose |
|---|---|---|
| `CMD_HI` | $DF0B | Selects the opcode class. Sticky — set once, then fire many opcodes in that class. |
| `CMD_LO` | $DF0C | Write an opcode number here to dispatch it. This is the write that halts the C64 until the command completes. |
| `STATUS` | $DF0D | bit0 busy, bit1 error, bit2 vblank-pending, bit3 vblank-IRQ-armed |
| `ERRCODE` | $DF0E | Written by every dispatch, success or failure — see [error-codes.md](error-codes.md) |
| `ID` | $DF0F–$DF10 | 16-bit resource id, for opcodes that create or reference one (textures, meshes, nodes...) |
| `ARG0`–`ARG15` | $DF11–$DF20 | 16-byte argument block for the opcode about to fire |

## What a command costs the C64

Writing `CMD_LO` halts the 6502 — like an REU transfer — until gpu64
finishes the command and writes `ERRCODE`. Most opcodes return in well
under a millisecond. Two are different and must not be called in a
per-frame loop: `VBLANK_SYNC` and `VBLANK_ARM` (when arming) can each halt
for up to a full frame, because they wait on a real vertical sync.

## Issuing a command

1. Set `CMD_HI` if you're not already in the right class (it's sticky, so
   skip this if the last command you sent was in the same class).
2. Stage the opcode's `ARG` bytes. Staging order is free — write them in
   whatever order is convenient. `ARG` registers are write-only and are
   **not** cleared between commands, so an opcode with fewer arguments than
   the last one you sent will still see old bytes in the ones it doesn't
   use — harmless, since it only reads what it's documented to read, but
   don't rely on `ARG` registers reading back as zero.
3. Write the opcode number to `CMD_LO`. The C64 halts here.
4. When it resumes, `ERRCODE` holds the result. `STATUS` bit1 is just
   `ERRCODE != OK`, for a one-instruction check.

## Blob descriptor

Two shapes, both little-endian, both naming a source or destination outside
gpu64's own address space:

**Full descriptor — 6 bytes.** Used wherever a transfer's length isn't
implied by other arguments (`BLIT`, `UPLOAD_TEXTURE`, `GET_INFO`, ...).

| Offset | Size | Contents |
|---|---|---|
| 0 | 1 | `space`: 0 = C64 RAM, 1 = REU |
| 1 | 3 | `addr`, 24-bit |
| 4 | 2 | `len`, 16-bit |

**Compact descriptor — 4 bytes.** `space` + `addr` only, no `len` — used
where the transfer size is implied by other operands (the matrix ops'
dimensions, for instance).

Both shapes double as a *destination*: for `GET_INFO`, `READ_RECT`, and the
matrix/vector ops, gpu64 writes its result to the memory the descriptor
points at, before the `CMD_LO` write that started the command returns.

Rules that hold for every descriptor, source or destination:

- `len = 0` (full descriptor) is a no-op — nothing is read or written.
- `addr + len` running past the end of its space is `OUT_OF_RANGE`; nothing
  is clamped.
- `space` outside 0/1 is also `OUT_OF_RANGE`.

## The demo programs

Ten C64 programs in [`Source/Demos/`](../Source/Demos/), each showing a
part of the API doing something you can look at. They are written to be
read: heavily commented, no shared cleverness beyond an include of the
register equates, and none of them depends on another.

This is not the conformance suite. The suite
([`Source/TestPRG/`](../Source/TestPRG/), run by `tools/testprg.sh`)
asserts that the firmware matches this reference and prints a verdict. The
demos assert nothing — they are what you copy from.

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
| `quake` (`gpu64_demo_quake`) | A driveable, first-person room you walk and look around in, past a static picture: W/S walk, A/D turn, Q/E strafe, F1/F3 look up/down, F7 to centre. Three monsters are **directional things** — `DRAW_THINGS` billboards with an eight-view texture set, one per 45° of facing, selected by the camera's position relative to each monster rather than by any angle math on either side. Projected through the full 3D camera (`SET_CAMERA3D`), not the 2D one `sectors` uses. | `SET_CAMERA3D`, `UPLOAD_VERTS`, `UPLOAD_TEXINFO`, `UPLOAD_POLYS`, `DRAW_WORLD`, `DRAW_THINGS`, `SET_VIEW`, `SET_COLORMAP`, `UPLOAD_TEXTURE`, `FILL_VIEW`, `RASTER_STATS`, `PAL_LOAD`, `PAGE_FLIP` |
| `text80` | The 80x50 character screen, which is a display **mode** and not a drawing op: while it is up there is no framebuffer, `PAGE_FLIP` answers `UNSUPPORTED` and so does every class 1 and class 2 command. One screen holds an 80-column ruler, all 256 screen codes uploaded a plane at a time, the sixteen palette colours as bars of reverse-video spaces, a reverse-video line built as raw screen codes to show what the ASCII flag does for you elsewhere, a per-frame marquee and a blinking cursor. It swaps the two halves of the character ROM every 128 frames, which is why the capitals keep turning into graphics -- that is what SHIFT+C= does on a real C64. It is the one demo that deliberately leaves its mode up on exit. | `TEXT_MODE`, `TEXT_CLEAR`, `TEXT_FILL`, `TEXT_WRITE`, `TEXT_UPLOAD`, `TEXT_PUT`, `TEXT_CHARSET`, `SET_BORDER`, `VBLANK_SYNC` |

Every one of them ends on RUN/STOP.

### Building and checking them

```
tools/demos.sh              # assemble, run on the PC, render all ten
tools/demos.sh -v rotate    # just this one, and print the C64 screen
```

Each demo is run twice under [`tools/prgsim`](../tools/prgsim/): once
modelling a display gpu64 measured a frame period from, and once modelling
one it could not, where every vblank feature answers `UNSUPPORTED`. A demo
has to survive both, which is why the runtime's `dmVbWait` / `dmFlip` fall
back to free-running and immediate flips instead of refusing to run.

What the run leaves in `Source/Demos/out/<name>.ppm` is what the HDMI
output was showing when the program returned — the visible page, through
the current palette, inside the border. **Look at those before deploying to
hardware.** Bench time is the scarce resource, and a demo that renders a
black rectangle on a PC will render a black rectangle on the bench.

### Running them on the C64

Copy the `.prg` files onto whatever the machine loads from, then

```
LOAD"GPU64-HELLO",8,1
RUN
```

In the RAD menu, press **T until it reads REU**. Everything gpu64 does
lives inside the REU polling loop; without that, nothing runs at all and
the HDMI screen keeps showing the text mirror.

### The two include files

- `gpu64_demo.inc` — the register window, the opcode numbers, the error
  codes, four macros for staging an argument block (`#argb`, `#argw`,
  `#argblob`, `#argcd`) plus `#cmd` to fire one, and — for class 2 — three
  more for staging batch records (`#r2col`, `#r2span`, `#r2batch`) and
  `#cmd2` / `#cmd0` to switch class and fire in one go. Copy this into your
  own project; it is the part of a gpu64 program that is the same in every
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

See also: [project/demos_status.md](../project/demos_status.md) for which
of these have actually run on real hardware, and when.
