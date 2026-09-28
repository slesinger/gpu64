# gpu64 demonstration programs

Sixteen C64 programs in [`Source/Demos/`](../Source/Demos/), each one showing a
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
- `walls` has been rendered on a PC and not separately checked at the bench;
- `stunt` has been rendered on a PC only;
- `nighthawk`'s flight and checkride flew at the bench on 2026-09-26; the
  strike mission has been rendered on a PC only.

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
| `game` | The step past `level`: the same E1M1, but the doors **open**. The C64 reads all 369 entities once through `LEVEL_ENT`, keeps a table of the movers and of the trigger and button volumes that fire them, and matches `target` to `targetname` as 16-bit string-pool ids — no string comparison on the 6502 anywhere. Walking into a trigger, or into a secret door, or onto a button, animates the door it names: `SET_POSITION` on the brush model's run of scene nodes, and the same displacement fed back into the next frame's `CLIP_MOVE` mover list, so a half-open door blocks by exactly half. W/S/A/D/Q/E walk, turn and strafe, left SHIFT runs, SPACE jumps; F1/F3/F7 pitch the view and +/- change the field of view (75° at start). E1M1's monsters and pickups stand in the level as real Quake models, posed each frame through `WORLD_TICK`, and the gun and status bar are drawn on the HDMI screen. C= or B fires (either counts only when the keyboard matrix cannot be ghosting it), and 1–4 select the axe, shotgun, super shotgun and nailgun. Shots are hitscan: the Pi traces line of sight to the nearest monsters in the cone and the C64 resolves the pellets. Grunts and dogs flinch and die, and a dead grunt drops a backpack. Health, armour, ammo and weapons are picked up by walking over them. ← is a test key that hurts the player by 20. A dead player's view sinks, and fire starts the level again. Joystick 2 walks (up/down), turns (left/right) and fires; while it is off centre the keyboard is ignored, because a held direction ghosts keys. A 1351 mouse in port 1 turns (sideways), looks up and down (forward/back) and fires with its left button. RUN/STOP is ignored: the game has no exit, and a reset leaves it. If the C64 freezes, RESTORE prints `NMI`, the address the 6502 was at and a press count on row 0; a crash into a BRK prints `BRK` and its address there and stops, instead of dropping to READY. Monsters do not move or attack yet. Rows 20–22 of the C64 screen are the actor block's telemetry, then `HP AR SH NL W K` and `SHOT HIT LOST PICK DIE`. A player the bus has put inside solid or below the map is put back where they last stood, and one a lift has shut through climbs out on top. Two refresh rings keep the retained scene honest against lost bus writes, one step a frame: the scene-wide state, and one level node via `LEVEL_NODE` with the palette via `LEVEL_PALETTE`. The camera, the moving doors and the node refresh are each written to the registers twice a frame, so a single lost write is overwritten before it can put the eye outside the room, and each carries the optional check byte (`ARG14`/`ARG15`), so one that arrives with a bit flipped is refused and sent again instead of drawn. | `LEVEL_ENT`, `LEVEL_NODE`, `LEVEL_PALETTE`, `CLIP_MOVE`, `WORLD_TICK`, `LOAD_LEVEL`, `LEVEL_STEP`, `SET_POSITION`, `SET_DMA_WINDOW`, `GET_HEALTH`, `LOOP_START`, `SCENE_COMMIT`, `MOVE_LOCAL`, `ROTATE_LOCAL` |
| `stunt` | A **game**, not a tour: Stunt Car Racer rebuilt on the retained scene. A figure-of-eight circuit on stilts with a bridge over its own crossing, a roller-coaster of humps and a ramp with a gap in it, raced over three laps against a rival car. Everything is uploaded once — sixteen track meshes, the ground, a ring of hills, twelve tree sprites, two cars — and a frame on the C64 is the physics plus seven small commands: two cars, the camera, the horizon, the commit. Fall off the side or short of the gap and the crane puts you back, with damage; hard landings dent the car too, and at 255 it is wrecked. W/S accelerate and brake, A/D steer, SPACE boosts (a tankful a lap), F1 swaps the chase and cockpit views, F3 hands the car to the autopilot — the same routine that drives the rival. The track, textures and the tables the physics reads it through are generated by `tools/gen_stunt.py`. | `UPLOAD_MESH`, `UPLOAD_TEXTURE`, `BUILD_COLORMAP`, `CREATE_OBJECT`, `CREATE_SPRITE`, `SET_SPRITE`, `CREATE_CAMERA`, `SET_POSITION`, `SET_ORIENTATION`, `SET_DMA_WINDOW`, `LOOP_START`, `SCENE_COMMIT` |
| `nighthawk` | A flight game in the spirit of *F-117A Stealth Fighter*, and the retained scene at a scale none of the others reach: a 25.6 km coastal theatre of hills, farms, towns, forest and two airbases, all in view at once, with no fog. The 6502 **builds the terrain itself** — 56 tiles of 9x9 vertices written from a height map and uploaded as meshes, 5196 triangles — and the same height map gives the flight model the ground under the wheels. The cockpit — the flight instruments, the EMV meter, weapons, messages and a moving tactical map — is drawn on the HDMI screen along the bottom of the view: the C64 writes it as text on its own screen and turns each changed 4-character piece into a texture. The rest of the C64's screen is a debug console and the bus report card. Joystick 2 or W/S/A/D flies, +/- is the throttle, G the gear, F1 swaps the chase view for the cockpit and its pitch ladder, and F3 hands the jet to an autopilot. A touchdown off the concrete, too fast, banked or gear-up is a crash, and the jet goes back to the runway. Keys 1–4 select the mission: 1 the checkride (take off, four waypoints, land); 2 the strike, the coastal radar past two early-warning radars and four SAM batteries; 3 the bridge, at night; 4 the factory, at night, with two MiGs on patrol over it and more scrambled from the enemy airfield once the alert is up. The radars see you by your signature, which the EMV bar shows: the base, plus open bay doors, gear down and full throttle. Staying below 150 m halves their range, and a hill in between hides you. SPACE opens the bay, FIRE or RETURN drops one of two bombs when the cue reads DROP, C drops chaff against a radar SAM and F a flare against a MiG's IR missile, both only useful at close range. M fires an AIM-9 at the MiG the HUD reads LOCK on, and F5 turns on the bomb cam, which rides the next bomb down to the target. Night is a low moon and a dark sky, with the runway and bridge lamps unlit geometry. The autopilot flies every mission end to end, including shooting down the factory's MiGs and flaring their missiles. | `UPLOAD_MESH`, `UPLOAD_TEXTURE`, `BUILD_COLORMAP`, `CREATE_OBJECT`, `CREATE_CAMERA`, `SET_ACTIVE_CAMERA`, `SET_POSITION`, `SET_ORIENTATION`, `SET_VISIBLE`, `SET_LIGHT3D`, `SET_BACKGROUND`, `SET_DMA_WINDOW`, `SCENE_COMMIT` |
| `text80` | gpu64's **other display mode**: 80 columns by 50 rows of 8x8 glyphs taken from the C64's own character ROM, at 640x400. Nothing is emulated and nothing is scaled — a screen code here means what it means at `$0400`. It is a display mode, not a drawing op: while it is up there is no framebuffer to draw into, and it stays up until `TEXT_MODE 0`. | `TEXT_MODE`, `TEXT_CLEAR`, `TEXT_PUT`, `TEXT_WRITE`, `TEXT_UPLOAD`, `TEXT_FILL`, `TEXT_CHARSET`, `SET_BORDER` |

Every one of them except `game` ends on RUN/STOP.

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
moved, and moved back again when the door shuts. `tools/check_combat.py` does the
same for the fighting: it shoots a grunt and requires pain, then a held death
frame, a backpack worth exactly five shells, and a death and restart that
puts the player back on the start without firing.

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
