# Milestone 20 — Quake combat: monsters, weapons, the rest of E1M1

Step 5c of [milestone 18](milestone18_quake_game_design.md), widened. The
game demo opens E1M1's doors; this milestone populates the level and makes it
winnable: monsters that see, chase, shoot and die, the player's weapons and
pickups, a Quake status bar and view model on HDMI, player death and restart,
`trigger_counter` and the exit, SID sound, and muzzle-flash lighting.

This is the as-designed record. Status goes in `progress_tracker.md`, and
anything a user of the API needs goes in `docs/`.

## Decisions taken with the user (2026-09-27)

| Question | Decision |
|---|---|
| Monster art | **Real MDL meshes**, one class-1 mesh per animation frame. Billboards only if resources force it; they do not (see budgets). |
| World queries (LOS, hitscan, monster moves) | **One new batched opcode** answering many traces per call |
| Per-frame actor state (mesh frame, position, yaw) | **The same batched opcode** — one checksummed block a frame, both directions |
| AI fidelity | **Quake-like, simplified**: sight/sound wake, chase, grunt hitscan with Quake's spread and damage, dog bite + leap, pain and death animations, corpses stay |
| Entity set | **Normal skill**: 18 grunts, 5 dogs; shotgun, super shotgun, nailgun |
| Weapons and pickups | **The E1M1 set**: axe, shotgun, super shotgun, nailgun; health, armor, shells, nails, quad, envirosuit |
| HUD and view model | **Both on HDMI**; the C64 screen keeps the telemetry rows |
| Extras | SID sound effects, player death and restart, keys/locked doors/counters, muzzle flash via point lights |

E1M1 has **no keys and no locked doors**. `trigger_counter` (the three
buttons in the big room) and `trigger_changelevel` (the exit) are what
"completable" needs. Key and lock logic will be written generically from the
entity fields, but E1M1 cannot exercise it, so it counts as unverified until
a level that uses keys is converted.

## What E1M1 asks for at normal skill

From the BSP's entity lump (`spawnflags & 0x700`, excluding not-on-medium
and deathmatch-only):

| Class | Count | Asset |
|---|---|---|
| `monster_army` | 18 | `progs/soldier.mdl`, 170 verts, 328 tris, 114 frames, skin 300×194 |
| `monster_dog` | 5 | `progs/dog.mdl`, 236 verts, 426 tris, 86 frames, skin 312×169 |
| `item_health` | 14 | `maps/b_bh25.bsp` / `b_bh10.bsp` (brush models) |
| `item_armor1/2` | 2 | `progs/armor.mdl`, 3 skins |
| `item_shells` / `item_spikes` | 3 / 2 | `maps/b_shell*.bsp`, `maps/b_nail*.bsp` |
| `weapon_supershotgun`, `weapon_nailgun` | 1 / 1 | `progs/g_shot.mdl`, `progs/g_nail.mdl` |
| quad, envirosuit | 1 / 1 | `progs/quaddama.mdl`, `progs/suit.mdl` |

View models: `v_axe` (98 verts, 9 frames), `v_shot` (68, 7), `v_shot2`
(86, 7), `v_nail` (109, 9). Nails: `progs/spike.mdl`.

**Every model fits the 256-vertex cap as it stands.** The class-1 face
format carries UVs per corner, so Quake's seam vertices need no duplication.
The dog is the tightest at 236.

## Budgets

**Pi resources (512 slots).** As built (stage A): the level is 65 textures
and 82 meshes; milestone 20 adds 78 textures and 162 meshes, for 143 + 244 =
387. The meshes are 114 monster frames (the grunt keeps stand, run, shoot,
pain, death and deathc, 52 of its frames; the dog keeps stand, run, attack,
leap, pain, death and deathb, 62), 32 view-model frames, 9 alias pickups
(three of them armor skins) and 7 brush pickups. The textures are 15 model
skins, 7 brush-item textures, 39 status-bar pics, 5 status-bar tiles and a
crosshair.

**Level file (2 MB buffer, 1350 KB used).** A frame is an ordinary mesh
record. What makes that affordable is that `gpu64_3dBuildMesh()` *copies*
its blobs into the arena, so any number of mesh records may point at one
face blob: a model is stored as one face blob plus 6 bytes per vertex per
frame (about 1–1.4 KB a frame), and **no firmware change and no new file
section** is needed. The earlier plan to store frames compactly and expand
them in the loader was dropped when this was found. Skins are resampled
to the nearest power of two on a log scale, at most 256, and are about
470 KB between them. `GPU64_LEVEL_MAX_BYTES` is now 2 MB. It is a BSS buffer
filled before the polling loop runs, so its size is not a timing question.

**Catalogue guard.** The game compiles ids in from
`Source/Demos/gpu64_quake_actors.inc`, which the converter writes. So the
level file carries a `gpu64_catalog` entity as its last record: kind 9,
spawnflags = a CRC of every symbol in the include, p0/p1 = the mesh and
texture counts. The game compares the entity against `CAT_ENT`/`CAT_HASH`
and refuses a level from another converter run.
`tools/check_g64lev.py` cross-checks the include against the file.

**Scene nodes (256).** World 108, monsters 23, items about 28, nails 8, view
model 1, HUD about 12, camera 1, lights 3. That is about 184.

**Point lights (8 live).** Muzzle flash 1, nail glow for the first 2 live
nails, quad glow 1. The rest stay free.

**C64.** The game is 12.5 KB of code today. Actor tables for about 60
actors at 32 bytes each come to 2 KB; AI, weapons, HUD and SID add perhaps
8 KB. RAM is not the constraint. **6502 time is.** AI thinks at 10 Hz as
Quake's does, round-robin over awake monsters, so a 30 fps frame runs a
third of them.

## The new opcode: `WORLD_TICK` ($6C)

*This section is the plan as written before stage B. Where the build
differs, "Stage B as built" below wins; the byte layout is in
`Source/Firmware/gpu64_3d.h`.*

The bus is the constraint, as in milestone 18. Per-actor `SET_POSITION` /
`SET_ORIENTATION` / mesh swaps would cost about 23 × 3 commands a frame, and
every `CREATE_*` re-issue widens the phantom-`CREATE_*` exposure
(`gpu64-phantom-create-level-node`). A per-trace `CLIP_MOVE` multiplies block
calls, and run 37 showed failures grow with traffic. So there is **one block
a frame** that carries everything in both directions, on `CLIP_MOVE`'s model:
a descriptor in ARG0-5, magic and XOR checksum on each half, the output half
poisoned by the C64 before each call, retry up to 4 times, bound every answer
by its question, and `SET_DMA_WINDOW` around the block.

```
header (16 bytes)
  0      magic $D8
  1      nActors   0-32
  2      nTraces   0-24
  3      nMoves    0-24
  4      nMovers   0-16   (doors, exactly CLIP_MOVE's mover records)
  5      frame     C64 frame counter, echoed back (staleness check)
  6-7    check     16-bit sum over header and all input records
  8-15   reserved
actor records (16 bytes each), in
  node id (2), resource id (2), flags (1), yaw (1), pitch (1), pad (1),
  x, y, z  position (encoding: see below)
trace records (in 16, out 8), hitscan / line-of-sight
  in:  start xyz, end xyz, hull, mode
  out: fraction 0-65535, contents, plane normal index, magic, check
move records (in 16, out 16), monster walkmove
  in:  start, delta, hull, mode (CLIP_MOVE mode bits)
  out: end, flags, magic, check
mover records (16 each), as CLIP_MOVE
output trailer
  magic $8D, frame echo, 16-bit sum over every output byte
```

The exact field widths are settled during implementation against the
level's coordinate range. E1M1 spans about ±3000 Quake units, which needs
more than a 16-bit integer part in gpu64 units at the level's scale, so the
position encoding follows whatever `CLIP_MOVE` and `SET_POSITION` already
use. Three properties are fixed now:

- **Actor records are absolute.** Node, resource, pose and flags are sent
  whole, never as deltas, and are applied **only if the entire input block
  checks**. A record is a full description of the node, so a lost block costs
  one frame of stale pose, never a permanent error. That is
  `gpu64-camera-must-be-absolute`, applied to every actor.
- **The resource field is the animation.** A mesh node gets a new mesh id,
  and a sprite node a new texture id. That is the mesh-swap the opcode table
  lacks, and it is how HUD digits change without `UPLOAD_TEXTURE` leaking
  arena (Nighthawk's `DEAD` failure mode). The Pi refuses a resource id of
  the wrong kind for the node and flags that record in the output. It does
  not create nodes: every node is created once at start-up, and afterwards
  the only way to change a node is through this opcode.
- **Actor flags:** visible, and **view-space**, meaning the pose is relative
  to the active camera and the node renders after the world with its own
  depth clear. That flag carries the gun and the status bar. Nighthawk
  re-posed its HUD on the camera from the 6502 every frame; a view-space
  flag makes that unnecessary.

Queries are **answered from the level file and write no scene state**, like
`CLIP_MOVE`. The actor half **does** mutate the scene, but only through
`sceneTarget()`, so it is shadow-redirectable like `SET_POSITION` and skips
the pre-execute drain (the plan said otherwise; see stage B). It applies the
actors, then answers the queries. Answers are consumed on the
next frame; one frame of AI latency is what Quake's 10 Hz think rate has
anyway.

**Firmware work:** `opWorldTick()` in `gpu64_3d_class1.cpp`, reusing
`gpu64_levelTrace()` / `gpu64_levelMove()`; a view-space pass in the render
path (`gpu64_3d_render.cpp`) drawn after world and sprites; and the larger `GPU64_LEVEL_MAX_BYTES` (done in stage A). The loader needs
no change: frames are ordinary mesh records sharing a face blob. Nothing here touches
`reuUsingPolling()` or either register decode path. It is dispatch work under
the existing hold.

**Keyed?** No. It destroys nothing, and a phantom `WORLD_TICK` without a
valid block magic and checksum does nothing. It goes through the checked
`stage2` path like every other command, so strict mode refuses a bit-flipped
opcode.

## The C64 side

Everything authoritative stays on the 6502, as in milestone 18: health,
ammo, monster state, damage and the rules.

- **Actor table.** Slot per monster, item, nail and view element: node id,
  kind, state, anim frame and timer, position (same format as the player's),
  yaw, health, target, attack cooldown. Each frame builds the actor half of
  the block from slots marked dirty, plus a round-robin refresh of two clean
  slots so a lost block heals by rotation (`gpu64-state-refresh-ring`).
- **Monster AI (10 Hz, round-robin).** `stand` → `walk/run` when the player
  is visible (LOS trace query, within the monster's forward 180°) or makes
  noise (a shot within a radius wakes everything that can reach). Chasing
  uses Quake's `movetogoal`: a move query toward the player, and if blocked
  try ±45°/±90° next think. Grunt: in range with LOS, 3-frame wind-up, then a
  4-pellet hitscan with Quake's spread and 4 damage per pellet. Dog: bite at
  melee range (8+rand damage), leap at mid range as a ballistic move.
  Pain chance and pain animation as in QuakeC; death picks one of two death
  sequences and leaves the corpse non-solid.
- **Player weapons.** Axe (a melee trace of 64 units); shotgun (6 pellets);
  super shotgun (14 pellets, 2 shells); nailgun (a projectile actor
  advanced each frame by a move query, 9 damage). The world fraction comes
  from the trace query and monster hits from a 6502 ray-against-box test
  against monsters nearer than the wall. Quad multiplies damage by 4.
  Pellets that hit the same wall share one trace; the batch holds up to 24.
- **Pickups.** Touch test against the item box (the door code's touch test).
  On pickup the node is hidden and the HUD updated. Nothing respawns,
  as in single-player.
- **Damage to the player.** Armor absorbs 30% (green) / 60% (yellow) as
  Quake does; red HUD face flash; death at 0.
- **Death and restart.** View drops to the floor over 1 s, "PRESS FIRE"
  appears on the status bar, then the restart resets the C64's tables and
  re-sends every actor as a full record. The level itself is not reloaded:
  doors go back through `LEVEL_NODE`, monsters and items through their actor
  records. That is a few frames, not a 29-step load.
- **Counters and the exit.** `trigger_counter` counts its `count` firings,
  then fires its targets. `trigger_changelevel` ends the run on a stats
  screen (kills / secrets / time) on HDMI and the C64 screen.
- **HUD on HDMI.** Quake's status bar art (`gfx.wad`: `sbar`, `num_0`–`num_9`,
  `num_minus`, the face frames, and ammo and armor icons) is converted into
  the level file as textures. The bar is view-space sprite nodes: 3 digits
  each for health, armor and ammo, plus face, ammo icon and armor icon. A
  digit change is a resource-id change on a view-space actor, so it costs
  nothing extra.
- **View model.** One view-space mesh node; weapon switch and firing are a
  resource change per frame. The idle bob is a small y offset in the record.
- **Muzzle flash.** One light node created at start-up, moved to the player
  and made visible for 2 frames per shot (the pattern `docs/` already
  recommends). Nails carry a small light while in flight, first two only.
- **SID.** A per-frame driver with a small effect table: shotgun, super
  shotgun, nail, axe swing, grunt and dog sight, pain and death, pickup,
  door, and player pain and death. It is ticked from the main loop, not from
  an IRQ, so it stays out of the DMA-hold timing. Three voices; a newer
  effect takes the oldest voice.
- **Controls added:** CTRL or joystick fire to shoot, 1-4 to pick a weapon.
  Joystick port 2 as an alternative to WASD is cheap and worth doing.

## Gates before any bench time

Per CLAUDE.md, everything here is verifiable on a PC first.

1. **Converter.** `tools/gen_quakelevel.py` gains the MDL, brush-item and
   `gfx.wad` sections. `tools/check_g64lev.py` validates them: vertex cap,
   UV span, frame ranges and power-of-two skins.
2. **Render check.** `tools/hostsim/levelsim` places one of each model at
   the player start in each animation, plus the HUD and view model, and
   renders to PPM. A wrong axis swap or winding shows up here, as it did for
   the level in milestone 18.
3. **Firmware on the host.** A `worldticktest` beside `hulltest` checks block
   acceptance and rejection (bad magic, bad sum, truncated), that actors are
   applied only on a full check, wrong-kind resource refusal, and that trace
   answers match `gpu64_levelTrace()`.
   *As built:* `opWorldTick()` does not compile on a PC, so
   `tools/worldticktest.py` runs the queries through the firmware's C and
   the protocol through prgsim's model of it. See "Stage B as built".
4. **The game under prgsim.** `tools/prgsim` models `WORLD_TICK` through the
   firmware's own code (the `libclipmove.so` pattern). `tools/check_game.py`
   gains assertions on a scripted route: a grunt wakes, the player takes
   damage, a grunt dies, a pickup raises health, the counter fires, and the
   exit is reached. It also runs under `--bus-fault` to confirm a dropped
   block costs a frame and never a permanent pose. Runs are several hundred
   frames, per `vice-validation-run-length`.
5. **Bench.** One session: C64 alive, `WORLD_TICK` rejection rate, the
   refresh-ring repair counters and frame rate on the telemetry rows,
   printed live (`gpu64-bench-instrument-print-live`).

## Order of work

| Stage | Content | Gate |
|---|---|---|
| A | Converter: MDL / brush-item / gfx.wad sections, frame selection, skin resampling | check_g64lev + levelsim PPMs |
| B | Firmware: `WORLD_TICK`, view-space pass | worldticktest, levelsim |
| C | C64: actor table + `WORLD_TICK` plumbing; place all monsters and items idle; HUD and view model static | prgsim/scenesim render |
| D | Weapons, hitscan, pickups, player damage and death, restart | check_game assertions |
| E | Monster AI: grunt, then dog | check_game route |
| F | Counters, exit, stats screen; SID; lights | check_game + manual listen in VICE |
| G | Bench | one session |

## Stage A as built (2026-09-27)

- `tools/quake_assets.py` holds the MDL, brush-item and `gfx.wad` sections.
  `tools/gen_quakelevel.py` calls it, and `--no-actors` gives the old,
  level-only file. `--inc` writes `Source/Demos/gpu64_quake_actors.inc`
  with `M_*` mesh indices (add `MESH_BASE`), `N_*` frame counts and `T_*`
  texture ids.
- **Actor local axes are Quake (x, y, z) → (−y, z, x) / 32.** The model's
  forward becomes local +z, so a node takes the entity's `LEVEL_ENT` yaw
  unchanged. The determinant is −1, as for the level's own swap, so no
  triangle is reversed. Vertices are *not* re-centred: a node's position is
  the entity origin. Brush pickups are re-expressed from the level axes into
  the same actor axes.
- **View models:** the origin is the eye. `v_shot`'s idle frames park the
  muzzle flash at x = −14 Quake units, *behind* the eye, and only the firing
  frame moves it to the muzzle. The stage-B view-space pass must therefore
  keep a positive near plane: with the near clip gone, the flame would draw
  on every frame.
- Skins: Quake's pure-blue padding (index 208) is bled into before the
  skins are resampled. Without that, `v_shot2` showed blue slivers along its
  seams.
- The status-bar art is remapped for sprites (255 → hole 0, black 0 → the
  darkest other index). Pics are centred in power-of-two textures, `SBAR`
  is five 64×32 tiles, and the crosshair is generated.
- **Gate:** `tools/check_g64lev.py`, which now also checks the catalogue
  against the include. Then `python3 tools/actorsheet.py`, which renders
  all 17 animations through levelsim into `tools/hostsim/out/actors/*.png`
  and fails on a blank frame. levelsim gained `--no-level`, `--bg=`,
  `--actor=MESH[+N]@x,y,z[,yaw]` and `--sprite=TEX@x,y,z,w,h`.
  Inspected by eye: the grunt and dog face their yaw (yaw 180 faces the
  camera), every model is solid from the front and the back, the animations
  step in order, and the armor skins, ammo boxes and health boxes all
  texture correctly.

## Stage B as built (2026-09-28)

- **Opcode `$6C`, not `$18`.** `$18` is one bit from `SCENE_COMMIT` and
  `UPLOAD_MESH`, and this is the command sent every frame. Every one-bit
  neighbour of `$6C` is undefined in class 1, and its class-0 twin (a lost
  `CMD_HI`) is undefined too: `LEVEL_NODE`'s rule, applied strictly.
- **Drain-exempt through the shadow.** `WORLD_TICK` is in
  `isShadowRedirectable()`, so the actors land in the shadow scene and the
  C64 never waits for a render. The plan had it draining, which would have
  halted the C64 once a frame, against the stability decision.
- **Layout (header comment is authoritative).** 16-byte header with a
  16-bit sum; actors 20 bytes (node, resource or `$FFFF`, flags, yaw,
  pitch and roll as high bytes, s32 16.16 position); traces and moves
  28 bytes in and 16 out; movers are `CLIP_MOVE`'s 16-byte records; output
  = 8-byte header (magic, frame, applied, refused bitmap) + answers +
  4-byte trailer (sum, magic, frame). Move answers are `CLIP_MOVE`'s out
  bytes 32-47 exactly.
- **All-or-nothing, then per record.** Everything that can refuse the
  block (magic, counts, length, sum, every hull, every mover model, a query
  with no level) is decided before any actor is applied. After that the
  only refusal is per actor record (no such node, wrong-kind resource, a
  resource on a light or camera, an undefined flag bit), reported in the
  bitmap and in `RESULT`.
- **Hull 0 exists now.** The level file is v5: each hull record gains
  `head0`, and the converter builds hull 0 the way Quake's `Mod_MakeHull0`
  does, from the BSP nodes. Without it a shot would be traced with the
  player's 32-unit box. `gpu64_levelTraceEnts()` answers a trace with the
  world and the movers, and the contents one unit behind the impact (SOLID
  when that is a mover's face). The firmware refuses a v4 file: copy the
  new `level.g64lev` to the card.
- **View-space pass.** `Gpu64_3dNode.viewSpace`, set only by
  `WORLD_TICK` and cleared by every `CREATE_*` and by `LEVEL_NODE`'s
  repair. `gpu64_3dSceneRender()` skips such nodes in the world passes, then
  (only if one is visible) clears depth with `gpu64_3dClearDepth()` (row
  chunks of the span budget, yielding between them) and draws them with
  the view transform set to identity. A view-space light is used in view
  space directly. The directional light is not re-rotated, so a gun is lit
  as if the sun turned with the player; the HUD should use unlit sprites.
- **The gun needs to sit forward.** At the eye (z = 0), the part of
  `v_shot` in front of levelsim's 0.25 near plane projects below the
  screen. Position it about 0.3-0.4 wu forward (the demo's near plane is
  0.1). A stage C/D number, not a firmware change.
- **Gate.** `python3 tools/worldticktest.py`: 32 checks on E1M1. The
  queries run the firmware's own C through `libclipmove.so`
  (`gpu64shim_trace_ents` is new); the protocol runs in prgsim's model
  (`c1_world_tick`). `opWorldTick()` itself does not build on a PC (the
  framebuffer and vsync headers), so its agreement with the model is by
  review; a disagreement shows at the bench as block rejections. levelsim
  gained `--view-actor=` / `--view-sprite=`. A view-space gun and HUD
  digits draw over a floor 0.3 wu from the eye, and view space and world
  space draw the same pixels for the same relative position. The frame
  stream's `N` line gained an optional 19th field, `viewspace`.

## Stage C as built (2026-09-28)

The C64 side of the actor table, in `Source/Demos/gpu64_game_world.inc`.
Monsters stand idle and pickups turn; nothing fights yet.

- **One slot table, fixed HUD slots first.** Slot 0 is the gun, 1-5 the
  status bar tiles, 6-18 the armour icon and digits, face, health digits,
  ammo icon and digits, and the crosshair; world actors start at slot 19
  (`SL_WORLD`), up to 80 slots. Each slot owns one scene node, created in
  setup with `CREATE_OBJECT` or `CREATE_SPRITE` + `SET_SPRITE`. `entTake`
  hands every entity kind past the trigger range to `actTake`, which maps
  the kind (and, for health, shells and nails, the spawnflags) to a mesh and
  skips entities with spawnflag 512 (not on normal skill). E1M1 gives 48
  world actors.
- **The HUD maps 1 pixel to 1/1024 of depth.** Every HUD sprite sits at
  z = 0.15625 / tan(fov/2), tabulated per FOV step, so one screen pixel is
  exactly 64 in 16.16: x = (col - 160) * 64, y = (100 - feetRow) * 64, and
  a 64-pixel texture is size 16 in 8.8. `+`/`-` re-send the HUD's z when the
  FOV changes. Sprites use flags `$0A` (unlit, depth-tested but not written).
- **The gun** is `v_shot` at scale 4.0, position (0, 0.4, 1.4) in view
  space. Lifts of 0 and 0.3 showed only the barrel tip; 0.5 crossed the
  status bar. It renders dark, because the directional light does not turn
  with the player (stage B). Left for stage F's muzzle light.
- **Dirty plus a refresh ring.** A slot is sent when it changed (pose,
  animation frame, digit) and, independently, two clean slots a frame are
  re-sent from a round-robin cursor. A block carries at most 30 dirty
  records; the rest wait for the next frame. The ring repairs a record the
  bus corrupted after it was accepted; the checksum refuses one corrupted
  on the way.
- **Refused records are re-created.** A record in the refused bitmap marks
  its slot `acFix`, and `actFrame` re-creates one such slot per frame (the
  node was destroyed by a phantom or never existed). **Not exercised on a
  PC:** no fault-injection run produced a refusal.
- **Retry the block, verify the answer.** Up to `WT_TRIES` (4) sends, the
  output area poisoned with `$A5` before each. An answer counts only with
  both magics, the frame number and the trailer sum right; otherwise the
  dirty flags stay set and the next frame sends the records again.
- **Animation at a quarter rate.** Idle animations and pickup rotation step
  only on frames where `(slot ^ frame) & ANIM_MASK == 0` (`ANIM_MASK` = 3),
  with the rotation step scaled to match. At every frame a block carried 19
  records and the frame took 53 ms under prgsim; at a quarter rate it carries
  about 9 and takes 46 ms, against 30 ms before stage C. About 13 ms is
  fixed cost: the slot scans, the 16-bit sum and `stage2`. prgsim charges 4
  cycles an instruction, so these are estimates; the bench will say.
- **The bug the gate found.** `sum16`'s page loop ended on `cpx #0`, which
  leaves carry set, so every page after the first added one: the C64 and the
  Pi disagreed on every block (`BD 007F`). A `clc` fixed it.
- **Telemetry, row 20:** `ACT N<records> OK <good blocks> BD <bad blocks>
  RF<refused records> FX<pending re-creates> <slots>`.
- **Gate.** `tools/demos.sh game`, including `check_game`; scenesim
  frames for the HUD and the gun; a relocated-camera render for the monsters
  and pickups; bus-fault runs (`drop:300`, `data:300`, frames 30-420 of 500)
  end with the same actor state as the clean run.

## Stage D as built (2026-09-28)

Weapons, hitscan, pickups, player damage, death and restart, in
`Source/Demos/gpu64_game_combat.inc`. Monsters still stand still and never
attack; that is stage E.

- **The Pi answers line of sight and nothing else.** A shot searches the
  actor table for up to `CB_NCAND` (3) live monsters inside a cone ahead of
  the eye, nearest first. The cone is `|lat| < fwd/4 + 128 fine` within
  `CB_RANGE` (64 wu); the axe's box is `fwd < 3 wu, |lat| < 160 fine`. Each
  candidate gets a point trace from the eye to its origin, in the same
  `WORLD_TICK` block as the frame's poses, with the frame's movers copied
  in. The answer's `TR_CLEAR` flag makes a candidate hittable. The pellets
  are then 6502 arithmetic against the forward and lateral distances the
  search already has. A pellet is `crandom() * spread` on both axes, and it
  hits the first visible candidate whose box it passes through. The box is
  ±128 fine wide and from −192 to +320 fine tall around the aim point.
  Damage is summed per monster before it is applied, as Quake's
  multidamage does. Vertical aim is automatic, as with `sv_aim`.
- **The E1M1 weapons at normal skill.**

  | Weapon | Damage | Frames between attacks | Ammo |
  |---|---|---|---|
  | Axe | 20 | 11 | none |
  | Shotgun | 6 × 4 | 11 | 1 shell |
  | Super shotgun | 14 × 4 | 15 | 2 shells |
  | Nailgun | 9 | 2 | 1 nail |

  The nailgun is hitscan. At E1M1's ranges a real nail lands a frame or two
  later, and drawing nails in flight would cost a node each. Quad damage
  multiplies damage by 4 for 650 frames.
- **Monsters.** A grunt has 30 hp and a dog 25. Pain plays the pain
  animation once (`AM_ONCE`), then the monster returns to stand. Death picks
  one of the two death animations at random and holds its last frame
  (`AM_HOLD`). A dead grunt drops a backpack worth 5 shells into a free
  slot.
- **Pickups.** A pickup is a bounding-box overlap: the item's 32 QU box
  against the player's, with the box's centre corrected for corner-origin
  items (health, shells, nails). Vertically, the item's origin must lie
  between 102 QU below the eye and 10 QU above it.

  | Item | Effect |
  |---|---|
  | Health | +25, or +15 for the rotten box, up to 100; left on the floor at ≥ 100 |
  | Megahealth | +100, up to 250 (no rot-down) |
  | Armour | Green, yellow and red absorb 77, 154 and 205 /256. Taken only if type × value beats what is worn |
  | Shells | +20 or +40, up to 100 |
  | Nails | +25 or +50, up to 200 |
  | Super shotgun | +5 shells and switches to it |
  | Nailgun | +30 nails and switches to it |
  | Quad | 650 frames of ×4 damage |
  | Suit | Taken; nothing hurts through it yet |

  The nailgun in E1M1 is deathmatch-only (spawnflags 1792), so it never
  spawns.
- **The player.** Damage is split between armour and health with Quake's
  ceiling rounding, and shows the pain face for 11 frames. At 0 hp the gun is
  hidden, the eye sinks 240 fine over 6 frames (`camDrop`, applied in
  `sendCamera`), and `deadKeys` releases every movement key. After 22 frames
  the fire key restarts: camera, movers, triggers, actors (backpacks freed,
  monsters respawned, pickups back) and the player. The press that restarts
  does not also fire: `fireHold = $ff` holds the weapon until the key is let
  go.
- **Keys.** 1–4 select the axe, shotgun, super shotgun and nailgun, if you
  own the weapon and have the ammo. CTRL or joystick 2's button fires. ←
  deals 20 damage to the player; it is a stand-in until stage E's monsters
  can attack.
- **Telemetry.** Row 21: `HP AR SH NL W K kills/total`. Row 22: `SHOT HIT
  LOST PICK DIE`. `LOST` counts shots whose block never verified, so their
  traces were never answered. It stays 0 under `drop:300`, `data:300` and
  `addr:300`: the block retry recovers every one.
- **Cost.** The pickup scan ran the full 24-bit delta on all 48 actors every
  frame, about 10 ms under prgsim. A prefilter on the whole-unit bytes of x
  and z brought it to about 4 ms. At a standstill the frame is now 52 ms
  under prgsim, against 46 ms at stage C.
- **Two assembler traps, both found by the gate.**
  - `quadDmg` used `beq ++` to reach its `rts`, but the second anonymous
    label was the `lda #$ff`. Every hit without quad did 255 damage, and
    every shotgun blast killed outright.
  - The "branch too far" fixes (invert, then `jmp`) first used anonymous
    `+` labels. Every earlier `bne +` whose target lay beyond one of them
    was captured, and a backpack gave 20 shells instead of 5.

  **Never insert an anonymous label mechanically.**
- **Gate.** `tools/check_combat.py` (12 checks, 32 s) now runs in `tools/demos.sh
  game`. The kill run turns to face grunt 245 and fires three shots. It checks
  pain, death on the second shot, a miss on the corpse, the death frame
  held, and the backpack taken for exactly 5 shells. The respawn run checks
  death after five ← presses, the gun hidden, and on restart the camera
  back on the start, full health, and no shot fired by the restarting
  press.
- **Not exercised on a PC.** The health, armour, ammo-box, super shotgun,
  quad and suit branches. None of those items can be reached from the start
  without a route search, and the one flooded route in `demos.sh` was not
  kept as a tool.

## Stage E as built (2026-09-28)

Monster AI for grunts and dogs, in `Source/Demos/gpu64_game_ai.inc`. The
split is the same as for the shot: the Pi answers geometry, and the C64
decides everything else.

- **Two questions, both in the frame's `WORLD_TICK` block.**
  - A **trace** from the monster's eye (origin + 25 QU) to the player's eye,
    hull 0: can it see the player?
  - A **move** from its origin, one step along its yaw, hull 1, in
    `CLIP_MOVE`'s walk mode (slide, step, floor settle, no eye offset):
    where does that step actually end?

  The monsters' traces follow the shot's and its moves follow every trace.
  `worldTick` sizes the block from `wtTrN + aiTrN + aiMvN`, which is at most
  3 + 4 + 6 = 13. That is inside the 15 that the one-byte trailer offset
  allows. Four monsters look per frame and six walk per frame. A
  round-robin start rotates who gets the budget, and a monster left out
  simply asks again on the next frame.
- **Asleep.** A monster wakes on either of two events.
  - **Sight:** on its think frame, the player is within 32 wu, in the
    half-space it faces, and the trace comes back `TR_CLEAR`.
  - **Noise:** any gunshot wakes every sleeper inside a 12 wu box.

  Being hurt also wakes it. With `--notarget` (`SIM_NOTARGET`), or while the
  player is dead, nothing wakes and nothing thinks.
- **Awake.** It thinks every other frame, which is Quake's 10 Hz.
  - Each think refreshes line of sight, turns at most 45° toward the
    player, and makes the attack decision.
  - It walks every frame: 40 fine a frame for a grunt, 72 for a dog.
  - It stops short of the player: 64 QU for a grunt, 40 for a dog.
    Monsters are not solid to each other or to the player.
  - A step that covers less than 160/255 of what it asked for marks the
    monster blocked. The next four thinks then detour 45° or 90° off the
    ideal yaw, alternating sides. This is a cheap `SV_NewChaseDir`.
  - A monster more than 64 wu away goes back to sleep.
- **A move answer is refused** in three cases:
  - it ends without `ONGROUND`;
  - it has `STARTSOLID` or `ALLSOLID`;
  - it ends more than one unit from where it started on any axis.

  The floor settle probes only one step height down, so the first case is
  Quake's "monsters do not walk off ledges" for free. The third is
  [bound the answer](../docs/state-refresh.md) again: the checksum refuses a
  corrupted block, and the bound refuses a nonsensical answer. `RF` on row
  23 counts refusals. In every PC run it has been 0, including under
  `drop:200`, `data:200` and `phantom:150`.
- **Attacks, normal skill.**
  - **Grunt** (Quake's `SoldierCheckAttack`). It needs to be in sight,
    within 768 QU, and past its cooldown. The chance per think is 0.9
    under 120 QU, 0.4 under 500 QU and 0.05 beyond.
    - The shoot animation fires on its fifth frame: 4 pellets of 4 damage.
    - A pellet hits when both `|crandom| × 0.1 × distance` offsets land
      inside the player's box (±16 QU lateral, ±28 QU vertical).
    - After the volley it rests 10 thinks.
  - **Dog.**
    - Within 80 QU it bites. On the attack animation's fourth frame the
      bite does 8 + 0..7 damage if the player is still within 100 QU.
    - Between 80 and 150 QU, in sight and past its cooldown, it leaps half
      the time: 8 frames at 128 fine a frame. Coming within 50 QU does
      10 + 0..7 damage once. A blocked leap ends where it hit.
    - After an attack it rests 6 thinks.
- **Animation.** Awake monsters animate every other frame (`AI_ANIM_MASK`)
  and sleepers every fourth. The end of an `AM_ONCE` animation goes to
  `aiAnimEnd`:
  - pain returns to the chase;
  - an attack sets the cooldown, then returns to the chase;
  - a sleeper returns to stand.
- **Restart.** `aiInit` saves each monster's spawn position and yaw once,
  after `combatCount`. `monSpawn` calls `aiRespawn`, which puts the monster
  back there, asleep.
- **Telemetry, row 23.** `AWK` awake now, `WK` wakes ever, `SEE` awake
  with a clear line, `BLK` blocked steps, `RF` refused moves, `HT` times a
  monster hurt the player.
- **The assembler trap stage D shipped with.** `acDelta`'s `.for a = 0 ...`
  loop left a symbol named `a` = 3 behind. From then on 64tass assembled
  every `lsr a`, `asl a`, `ror a` and `rol a` as a zero-page shift of `$03`,
  which leaves A alone, and it gave no warning. Twenty-four were
  mis-assembled, including all of `rnd`, `mul8` and `dmHexA`. Stage D's
  "random" pellet spread was therefore a fixed sequence, and armour
  absorption was wrong. The loop variable is `ax` now, and `tools/demos.sh`
  fails any demo whose listing has an accumulator shift that did not
  assemble to its one-byte opcode. It was found because the grunt's first
  volleys missed 12 pellets out of 12.
- **runsim `--warp=F:X,Y,Z,YAW`.** From frame F the game restarts with its
  eye at X Y Z, facing YAW. runsim writes `SIM_WARP_ON` and the position to
  `$02A9`/`$02B0`, and `simWarp` points `startCam` there, then calls
  `restart`. Only runsim writes those bytes; the game clears them at start.
  This is how a check reaches the dog room without a flooded route.
- **Gate.** `tools/check_ai.py` (12 checks, about 65 s) runs in `tools/demos.sh
  game` after `check_combat`. It has three runs:
  - **grunt:** the door route. Grunt 245 must wake, leave its spawn point on
    its own floor, and shoot the player down.
  - **dog:** a warp beside dog 247. It must close from z 47.5 to about 41,
    rising onto the room's 0.5 wu step exactly as the player does, and the
    grunt beside it must wake too.
  - **restart:** grunt 245 must be back on its spawn point and every
    monster asleep.

  Every other game run keeps `--notarget`, so it tests what it tested
  before.
- **Not modelled.**
  - Monsters never open doors.
  - A monster's move is traced only against the movers selected near the
    player.
  - Monster-versus-monster infighting.
  - Spawnflag 1 (ambush: sight only, deaf).
  - Pain chance: every hit that does not kill plays pain, as in stage D.

## Risks

- **Frame rate.** The game renders at about 32 fps on the bench. A room of
  five grunts adds about 1600 lit, textured triangles. If that costs too
  much, a mesh LOD, meaning the converter decimating monster frames used past
  some distance, is converter-only work.
- **Block size and bus exposure.** A full block is about 1.2 KB in and
  0.6 KB out a frame, and the transfer is where the known DMA defects
  live. Checksums catch the damage, but a high rejection rate costs frames.
  Only dirty actors are sent, and traces are capped per frame.
- **6502 time.** Ray-box tests for 14 pellets against nearby monsters, plus
  AI, can blow the frame. Mitigations: monster boxes culled by the wall
  fraction first; AI spread across frames.
- **Keys** stay unverified, as noted above.
