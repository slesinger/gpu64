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

## The demos

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
| `quake` | The step past `sectors`, and the geometry a Doom renderer cannot draw at all: a **ramp** you walk up, which is neither a wall nor a flat; a **bridge** with a walkway on top and an underside below, so there is floor above floor; and monsters with a front and a back. Class 2 arbitrary polygons, uploaded once and then addressed by face range. | `UPLOAD_VERTS`, `UPLOAD_POLYS`, `UPLOAD_TEXINFO`, `DRAW_WORLD`, `SET_CAMERA3D`, `SET_LIGHT`, `DRAW_THINGS`, `DRAW_SPRITE`, `FILL_VIEW`, `SET_VIEW`, `SET_COLORMAP`, `STATS` |
| `quake3d` | The same room again, as a **retained scene**: the level is a mesh node, the monsters are sprite nodes, the torch and the muzzle flash are light nodes, and the camera is a node too. Nothing here issues a draw call at all — `LOOP_START` hands the framebuffer to gpu64, which renders on core 1 at its own frame clock, and a frame on the C64 is a camera update and a commit. | `UPLOAD_MESH`, `UPLOAD_TEXTURE`, `BUILD_COLORMAP`, `CREATE_OBJECT`, `CREATE_SPRITE`, `CREATE_LIGHT`, `CREATE_CAMERA`, `SET_POSITION`, `SET_ORIENTATION`, `SET_VISIBLE`, `LOOP_START`, `SCENE_COMMIT` |
| `level` | The proof the retained scene scales: **a real Quake level, E1M1** — 620 KB, 66 textures, 15601 triangles — built off the Pi's own SD card by two commands the C64 sends in about forty register writes. The build costs 29 `LEVEL_STEP` calls with a loading bar; flying through it afterwards costs one camera update and one commit per frame, exactly what `quake3d` costs for its single room. The player walks: `CLIP_MOVE` traces each step against the level's Quake clip hulls, so you stand on the floor, climb the stairs, slide along walls and cannot leave the map. | `LOAD_LEVEL`, `LEVEL_STEP`, `CLIP_MOVE`, `SCENE_RESET`, `SET_VIEWPORT`, `SET_PERSPECTIVE`, `SET_BACKGROUND`, `SET_ACTIVE_CAMERA`, `LOOP_START`, `SCENE_COMMIT`, `MOVE_LOCAL`, `ROTATE_LOCAL`, `ARENA_STATUS` |
| `game` | The step past `level`: the same E1M1, but the doors **open**. The C64 reads all 369 entities once through `LEVEL_ENT`, keeps a table of the movers and of the trigger and button volumes that fire them, and matches `target` to `targetname` as 16-bit string-pool ids — no string comparison on the 6502 anywhere. Walking into a trigger, or into a secret door, or onto a button, animates the door it names: `SET_POSITION` on the brush model's run of scene nodes, and the same displacement fed back into the next frame's `CLIP_MOVE` mover list, so a half-open door blocks by exactly half. W/S/A/D/Q/E walk, turn and strafe, left SHIFT runs, SPACE jumps; F1/F3/F7 pitch the view and +/- change the field of view (75° at start). A player the bus has put inside solid or below the map is put back where they last stood, and one a lift has shut through climbs out on top. Two refresh rings keep the retained scene honest against lost bus writes, one step a frame: the scene-wide state, and one level node via `LEVEL_NODE` with the palette via `LEVEL_PALETTE`. The camera, the moving doors and the node refresh are each written to the registers twice a frame, so a single lost write is overwritten before it can put the eye outside the room. | `LEVEL_ENT`, `LEVEL_NODE`, `LEVEL_PALETTE`, `CLIP_MOVE`, `LOAD_LEVEL`, `LEVEL_STEP`, `SET_POSITION`, `SET_DMA_WINDOW`, `GET_HEALTH`, `LOOP_START`, `SCENE_COMMIT`, `MOVE_LOCAL`, `ROTATE_LOCAL` |
| `text80` | gpu64's **other display mode**: 80 columns by 50 rows of 8x8 glyphs taken from the C64's own character ROM, at 640x400. Nothing is emulated and nothing is scaled — a screen code here means what it means at `$0400`. It is a display mode, not a drawing op: while it is up there is no framebuffer to draw into, and it stays up until `TEXT_MODE 0`. | `TEXT_MODE`, `TEXT_CLEAR`, `TEXT_PUT`, `TEXT_WRITE`, `TEXT_UPLOAD`, `TEXT_FILL`, `TEXT_CHARSET`, `SET_BORDER` |

Every one of them ends on RUN/STOP.

## Building and checking them

```
tools/demos.sh              # assemble, run on the PC, render every one
tools/demos.sh -v rotate    # just this one, and print the C64 screen
```

Each demo is run twice under [`tools/prgsim`](../tools/prgsim/): once
modelling a display gpu64 measured a frame period from, and once modelling
one it could not, where every vblank feature answers `UNSUPPORTED`. A demo
has to survive both, which is why the runtime's `dmVbWait` / `dmFlip` fall
back to free-running and immediate flips instead of refusing to run.

`game` is the one demo that also *asserts*. A door that renders open because
its texture says so looks exactly like one whose scene node moved, so a
picture cannot judge it: `tools/check_game.py` reads the scene back out of
the sim and requires that the node on the door's travel line has actually
moved, and moved back again when the door shuts.

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
