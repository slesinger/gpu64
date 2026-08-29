# Class 1 — 3D mesh reference

This is what a byte means for `CMD_HI = 1`, gpu64's retained/immediate-mode
3D mesh pipeline: textured, affine-mapped triangles, flat-lit per face,
over a real z-buffer. **This is gpu64's general-purpose 3D layer — the
"OpenGL for the C64" one** — not a pipeline scoped to any one game genre;
a Doom/Quake-style first-person renderer is one thing it can draw, not
what it's for. It supersedes [class 2](api_design.md#class-2-opcodes--the-raster-layer)
(deprecated, frozen but still working) as gpu64's forward path for 3D.

See also: [project/milestone6_3d_design.md](../project/milestone6_3d_design.md)
for the architecture rationale (why a second core, why a generated colormap
instead of RGB, the store-burst budget) and the phase-1 build history, and
[project/gap_filling_plan.md](../project/gap_filling_plan.md) for what is
staged next to close the gap between this section and "Status" below.

## Status: retained scene graph, no autonomous loop yet

**Read this before the opcode table below — it changes what several rows
actually do today.**

The design targets a retained scene graph rendered autonomously by a second
core, with the C64 only ever moving nodes and committing frames. As of
stage 14 (2026-08-29), the scene graph itself is built and live; the
autonomous loop is not:

- The renderer, including `DRAW_NODE`, executes **synchronously on core 0**,
  inside the same DMA-halt window as any class 0 draw op — not on a second
  core. Node state (position, orientation, scale, visibility) is retained
  across commands; drawing it is still an explicit per-frame call.
- `LOOP_START` and `SCENE_COMMIT` ($06, $08) answer **`UNSUPPORTED`** — the
  autonomous loop and shadow-scene double-buffering are not built yet.
- Scene-node and transform opcodes, **$20-$24 and $30-$36**, are **live**:
  a node created with `CREATE_OBJECT`/`CREATE_CAMERA` persists in a 256-node
  table until `DESTROY_NODE` or a session reset, and `DRAW_NODE` ($42) draws
  it using whatever the transform opcodes last set — no re-staging a
  position/orientation/scale on every draw the way `DRAW_MESH` needs.
- `ARENA_STATUS` ($09) exists and was not in the original design sketch —
  see below.

In other words: **today, a class 1 program can either call `DRAW_MESH`
directly (transform argument staged fresh every call, nothing retained), or
build persistent nodes once with `CREATE_OBJECT`/`CREATE_CAMERA` and move
them with `SET_POSITION`/`MOVE_LOCAL`/`SET_VISIBLE`/etc. before calling
`DRAW_NODE` — but there is still no autonomous loop:** every frame's draws
are explicit calls from the C64, each one halting it for the duration, same
as `DRAW_MESH`.

Once the loop is live (staged in
[project/gap_filling_plan.md](../project/gap_filling_plan.md)), a
program's per-frame work collapses further: one small command per object
that actually moved this frame, then `SCENE_COMMIT` — a single
fire-and-forget trigger that publishes the frame and returns immediately;
the C64 never blocks waiting for the render, and finds out the next frame
is ready by polling `STATUS` bit4 rather than by waiting on the command
itself.

## Opcode table

`CMD_HI = 1`. `ARG` offsets are relative to `ARG0` ($DF11), exactly as in
class 0, and every opcode reads exactly the byte count in its row.

### System and loop — $00-$0F

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $00 | `SCENE_RESET` | 0 | — | Destroys every node, stops the loop, leaves uploaded resources alone. |
| $01 | `SET_VIEWPORT` | 8 | x, y, w, h (16-bit each) | Places the 3D viewport in the page. `w*h*3 > GPU64_3D_BUDGET` (196608, provisional) is `OUT_OF_RANGE`; a viewport not wholly inside 320x200 is `BAD_ARGS`. |
| $02 | `SET_PERSPECTIVE` | 6 | `ARG0-1` fov (binary angle), `ARG2-3` near, `ARG4-5` far (8.8) | Projection for the active camera. |
| $03 | `SET_LIGHT` | 7 | `ARG0-5` direction x,y,z (8.8), `ARG6` ambient level 0-15 | The single directional light. Face shade = ambient + N.L, clamped to 0-15, indexing the colormap. |
| $04 | `BUILD_COLORMAP` | 0 | — | Regenerates the 16-level colormap from the current palette. Needed after a palette change; costs milliseconds — never call it per frame. |
| $05 | `SET_BACKGROUND` | 1 | `ARG0` palette index | What the viewport is cleared to. |
| $06 | `LOOP_START` | 1 | `ARG0` 0 = handshake, 1 = free-running | **`UNSUPPORTED` in phase 1.** Design: starts the autonomous render loop; `NO_CAMERA` if no camera is active. |
| $07 | `LOOP_STOP` | 0 | — | **`UNSUPPORTED` in phase 1.** Design: stops the loop after the current frame. |
| $08 | `SCENE_COMMIT` | 0 | — | **`UNSUPPORTED` in phase 1.** Design: publishes the shadow scene; in handshake mode also flips at vblank and releases the next frame. `RESULT` = the page the just-finished frame is in. |
| $09 | `ARENA_STATUS` | 0 | — | `RESULT` = free resource RAM in 128 KB units (0..256 for the 32 MB phase-1 arena). Not in the original design; added because "you get `OUT_OF_MEMORY` eventually" is truthful and undebuggable without it. |

### Resources — $10-$1F

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $10 | `UPLOAD_MESH` | 12 | `ARG0-5` vertex blob descriptor, `ARG6-11` face blob descriptor | Uploads into resource RAM under the staged `ID`. Re-upload to a live ID replaces it in place — see Resource lifecycle below. `RESULT` = face count. |
| $11 | `UPLOAD_TEXTURE` | 8 | `ARG0-5` blob descriptor, `ARG6` w shift, `ARG7` h shift | Dimensions are `1 << shift`, 3..8 (8 to 256 px, power of two). `len` must equal `w*h`; anything else is `BAD_ARGS`. |
| $12 | `FREE_RESOURCE` | 0 | — | Frees the staged `ID`'s table slot. In phase 1 this does not reclaim arena bytes — see Resource lifecycle. |

### Scene nodes — $20-$2F

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $20 | `CREATE_OBJECT` | 2 | `ARG0-1` mesh resource ID | Creates a node instancing that mesh, under the staged node `ID`. Re-creating over a live `ID` replaces it — same convention as the resource table's re-upload. `OUT_OF_MEMORY` if the 256-node table is full. |
| $21 | `CREATE_CAMERA` | 0 | — | Creates a camera node under the staged `ID`. Same replace-on-live-ID and `OUT_OF_MEMORY` behaviour as `CREATE_OBJECT`. |
| $22 | `DESTROY_NODE` | 0 | — | Destroys the staged `ID`. `BAD_ID` if it does not exist. Destroying the active camera clears it — a later node reusing that same numeric `ID` is not silently treated as the camera again. |
| $23 | `SET_ACTIVE_CAMERA` | 0 | — | The staged `ID` becomes the camera `DRAW_NODE` and the loop render from. `BAD_ID` unless it names a live camera node. |
| $24 | `SET_VISIBLE` | 1 | `ARG0` 0 or 1 | Skips the node without destroying it. Any other `ARG0` value is `BAD_ARGS`. |

### Transforms — $30-$3F

All act on the staged node `ID` (object or camera — both carry a
position/orientation), and all write into the retained node table, not a
shadow copy — an immediate-mode `DRAW_NODE` sees the change on its very next
call.

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $30 | `SET_POSITION` | 12 | x, y, z (16.16) | Absolute, world space. |
| $31 | `SET_ORIENTATION` | 6 | yaw, pitch, roll (binary angle) | Absolute — replaces, does not compose with, whatever was there. |
| $32 | `MOVE_LOCAL` | 6 | dx, dy, dz (8.8) | Translate along the node's own axes, i.e. the delta is rotated by the node's current orientation before being added — "forward 1.5" means forward for *this* node, however it's currently facing. |
| $33 | `MOVE_WORLD` | 6 | dx, dy, dz (8.8) | Translate along world axes — added directly, no rotation. |
| $34 | `ROTATE_LOCAL` | 6 | dyaw, dpitch, droll (binary angle) | Added to the current yaw/pitch/roll independently, each wrapping mod 65536 — "add a turn rate and let it wrap", same as any other binary-angle field. |
| $35 | `SET_SCALE` | 2 | s (unsigned 8.8) | Uniform. Zero is `BAD_ARGS` — same rejection `DRAW_MESH` makes at draw time, caught here instead so the opcode that caused it is the one that fails. |
| $36 | `GET_TRANSFORM` | 6 | `ARG0-5` destination descriptor | Writes 18 bytes — position (12, s32 16.16 x/y/z) + yaw/pitch/roll (6, u16 each), all little-endian — back to C64 RAM or REU, for collision and gameplay logic. Reads back the accumulated angles, including anything `ROTATE_LOCAL` has added since the last `SET_ORIENTATION`. Destination `len < 18` is `BAD_ARGS`. |

### Immediate mode — $40-$4F

Legal only with the loop stopped — which today means always, since the loop
cannot be started. Otherwise `BUSY`.

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $40 | `CLEAR_VIEWPORT` | 0 | — | Fills the viewport with the background index and clears the z-buffer. Call this once per frame before drawing — nothing else clears the z-buffer for you (see Depth buffer below). |
| $41 | `DRAW_MESH` | 14 | `ARG0-5` position x,y,z (8.8), `ARG6-11` orientation, `ARG12-13` scale; mesh resource in `ID` | Transforms, lights, clips and rasterises one mesh into the draw page's viewport, z-tested. Runs synchronously on core 0 and returns when done. Ignores the scene graph entirely — draws in world space unless a camera has been applied by a preceding `DRAW_NODE` call this session (`SET_PERSPECTIVE`'s projection still applies either way). `RESULT` = triangle count drawn, saturated to a byte — **a mesh that draws 0 triangles and a winding-order mistake that culls every face look identical**, so check `RESULT` after your first upload of any new mesh. |
| $42 | `DRAW_NODE` | 0 | — | Draws the staged object node `ID` using its retained position/orientation/scale — no re-staging a transform, unlike `DRAW_MESH`. Applies the active camera (if any) fresh on every call, so moving the camera between two `DRAW_NODE`s in one frame is seen by both. `BAD_ID` if the staged `ID` is not a live object node (a camera `ID` included). An invisible node (`SET_VISIBLE 0`) is skipped silently: `RESULT` = 0, `ERRCODE` = `OK`, same "0 is informative, not an error" convention `DRAW_MESH` uses for full culling. |

## Mesh format

The C64 never touches mesh bytes — it points a blob descriptor at data
already sitting in RAM/REU, built by an offline exporter. Two blobs, fitting
the standard two-descriptor `ARG` layout used by `UPLOAD_MESH`:

```
blob 0 — vertices, 6 bytes each, at most 256 per mesh
    x, y, z            signed 8.8, model space (model fits ±128 units)

blob 1 — faces, 12 bytes each, triangles only
    i0, i1, i2         1 byte each, index into blob 0
    u0,v0, u1,v1, u2,v2  1 byte each, per-corner texcoords
    texid              1 byte, low byte of a texture resource ID
    flags              1 byte: bit0 double-sided, bit1 flat-colour
                       (texid is a palette index instead), bit2 unlit
```

Vertex and face counts are implied by each blob's `len` (`len/6`, `len/12`);
a `len` not a multiple of the stride is `BAD_ARGS`.

Notes that affect how you author or export a mesh:

- **Triangles only** — a quad exporter must triangulate. Two triangles cost
  one extra index byte and one extra flags byte versus a quad primitive; the
  format trades that for a single rasteriser path.
- **UVs are per face-corner, not per vertex.** This is required for boxy
  geometry — a cube corner needs three different UVs for the three faces
  meeting there. Don't try to dedupe vertices across faces with different
  UVs; the format already expects one UV set per triangle corner.
- **Normals are computed GPU-side from winding order at upload time** — you
  do not supply them, and there is no smooth-shading mode. Every mesh is
  flat-shaded per face.
- **256 vertices is per mesh, not per scene.** An index byte is relative to
  that mesh's own vertex blob. Split large geometry across multiple meshes
  rather than trying to raise this limit — it also makes per-object culling
  (once frustum culling exists) actually useful, since a giant single mesh
  is all-or-nothing against the frustum.

| Limit | Value | Set by |
|---|---|---|
| Vertices per mesh | 256 | the 1-byte face index |
| Meshes and textures resident | thousands | flat 16-bit resource ID space, 512 MB resource RAM (32 MB in the phase-1 arena) |
| Object instances in the scene | 256 | the scene table |

## Texture format

Power-of-two dimensions, 8 to 256 px on each side, not necessarily square,
8bpp palette indices — the same palette as class 0 and class 2. Power-of-two
is what lets the rasteriser wrap `u`/`v` with a mask instead of a modulo.
256 is the ceiling the byte-sized face-record UVs already imply; any other
size is `BAD_ARGS`.

## Resource lifecycle

- **Re-upload to a live ID replaces it** — implicitly frees the old
  allocation first, so "reload this texture" needs no free-then-upload
  dance.
- **`FREE_RESOURCE` is explicit; nothing is auto-evicted.**
- **All resources are freed on a session reset** (RUN/STOP+RESTORE or
  equivalent) — don't assume an ID survives across program runs.
- **IDs are one flat 16-bit namespace** shared by every resource kind
  (textures, meshes). IDs are chosen by the C64 side; nothing detects two
  pieces of code picking the same ID.
- **Phase 1: the arena is bump-only.** `FREE_RESOURCE` and re-upload reclaim
  the resource table's slot but not the underlying bytes — the arena itself
  only comes back at session reset. A program that re-uploads the same
  resource in a loop (e.g. streaming texture updates) *will* run the 32 MB
  arena dry and get `OUT_OF_MEMORY`, which is a truthful error, not a bug to
  work around. Use `ARENA_STATUS` ($09) to watch the free space if your
  program uploads/re-uploads a lot in one session.

## Lighting

One directional light (`SET_LIGHT`), shaded per face: `ambient + N·L`,
clamped to 0-15 and used as an index into a 16-level generated colormap
(`BUILD_COLORMAP`) rather than as an RGB multiply — the framebuffer stays
8bpp indexed all the way to scanout, same as everywhere else in gpu64.
Level 15 is full brightness (the identity mapping); level 0 is **1/15 of
the way up, not black** — a face turned fully away from the light still
reads as its own colour, not as a silhouette. Call `BUILD_COLORMAP` once
after any palette change; it costs milliseconds and must never be called
per frame.

Face `flags` bit2 (unlit) skips this entirely for that face and draws its
texture (or flat colour, if bit1 is also set) at full brightness — useful
for UI-ish geometry rendered through the 3D pipeline, e.g. a cockpit panel
that shouldn't dim when the scene does.

## Depth buffer

A 16-bit z-buffer sized to the viewport, storing `near/z` — **larger values
are nearer**, and an untouched pixel (value 0) loses every depth compare, so
it behaves correctly as "nothing drawn here yet" without a separate clear
value. `CLEAR_VIEWPORT` ($40) is what clears both the colour and the
z-buffer; nothing else does, so call it once per frame before your draws.

Depth precision is controlled entirely by `near` (from `SET_PERSPECTIVE`):
half the buffer's numeric range is spent between `near` and `2*near`,
wherever that is. Setting `near` unnecessarily small to avoid clipping
trades that away as z-fighting everywhere else in the scene — set `near` as
far out as the closest thing the camera will ever actually get to.

## Coordinate and rotation conventions

- **Axes are left-handed**: +x right, +y up, +z away from the camera.
- **Winding is clockwise as seen from outside the model.** A face is
  front-facing when its normal (from `cross(v1-v0, v2-v0)`) points back
  towards the camera. Get this backwards and every face of a closed mesh is
  culled — which looks exactly like a mesh that never uploaded. This is why
  `DRAW_MESH`'s `RESULT` is a triangle count: check it.
- **Positive pitch tips a node's own +z towards -y** — a camera with
  positive pitch looks *down*.
- Angles are a 16-bit binary angle (65536 = 360°): add a turn rate and let
  it wrap, no clamp or degrees/radians conversion needed. Positions are
  signed 16.16 (±32768 units at 1/65536 — a whole level, not just one
  model); per-frame deltas (`MOVE_LOCAL`/`MOVE_WORLD`/`ROTATE_LOCAL`) are
  8.8, matching everything else's world-unit convention.
- Internally, orientation is composed in 1.15 fixed point, not 8.8 — that
  detail is invisible at the wire format, but it's why a model turns
  smoothly instead of visibly wobbling: an 8.8 rotation matrix quantizes to
  1/256, which at 320px puts a rotating box's edge a whole pixel off its
  true position, and the error is systematic rather than random, so it
  reads as wobble rather than noise.
- **`near`/`far` (`SET_PERSPECTIVE`) are 8.8, so the far plane cannot exceed
  127.99 units.** This is a *scale* convention, not a hard distance ceiling
  — author your scene so the far plane fits, exactly as a mesh is authored
  to fit its own ±128-unit vertex space. Nothing stops "one unit" from
  meaning a kilometre; the unit just has to be chosen once, for the whole
  scene, up front.

## Register additions

Class 1 adds one readable byte beyond the class 0 register set:

| Address | Name | Dir | Purpose |
|---|---|---|---|
| $DF21 | `RESULT` | R | Low byte of the last command's result — page number from `SCENE_COMMIT` (design only), triangle count from `DRAW_MESH`/`DRAW_NODE`, face count from `UPLOAD_MESH`. Meaning is per-opcode; undefined for opcodes that define none. `CREATE_OBJECT`/`CREATE_CAMERA` don't allocate an ID and so don't set `RESULT` — node IDs are chosen by the C64 side, same as resource IDs. |

and one `STATUS` bit:

| Bit | Meaning |
|---|---|
| 4 | frame-ready — the loop has finished a frame and is waiting for `SCENE_COMMIT` (handshake mode only; not reachable yet, since the loop cannot start). |

New `ERRCODE` values this class adds: `OUT_OF_MEMORY` (resource RAM
exhausted), `QUEUE_FULL` (the core-0/core-1 command ring is full), `BAD_ID`
(no such resource or node), `NO_CAMERA` (a render was asked for with no
active camera). See [error-codes.md](error-codes.md) for the shared table.
