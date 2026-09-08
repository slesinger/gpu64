# Class 1 — 3D mesh reference

This is what a byte means for `CMD_HI = 1`, gpu64's retained/immediate-mode
3D mesh pipeline: textured, affine-mapped triangles, flat-lit per face,
over a real z-buffer. **This is gpu64's general-purpose 3D layer — the
"OpenGL for the C64" one** — not a pipeline scoped to any one game genre;
a Doom/Quake-style first-person renderer is one thing it can draw, not
what it's for. It supersedes [class 2](class2-raster-reference.md)
(deprecated, frozen but still working) as gpu64's forward path for 3D.

See also: [project/milestone6_3d_design.md](../project/milestone6_3d_design.md)
for the architecture rationale (why a second core, why a generated colormap
instead of RGB, the store-burst budget) and the phase-1 build history, and
[project/gap_filling_plan.md](../project/gap_filling_plan.md) for what is
staged next to close the gap between this section and "Status" below.

## Status: retained scene graph, real core-1 overlap, handshake loop live

**Read this before the opcode table below — it changes what several rows
actually do today.**

The design targets a retained scene graph rendered autonomously by a second
core, with the C64 only ever moving nodes and committing frames. As of
stage 16 (2026-08-29), that is live for **handshake mode**:

- `CLEAR_VIEWPORT`/`DRAW_MESH`/`DRAW_NODE` run on **core 1**, not core 0 —
  `gpu64_3dDispatch()` queues the draw and returns without waiting for it to
  finish, so the C64 is free to issue its next command while core 1 is still
  rasterising. `RESULT`/`ERRCODE` for these three catch up to the draw's own
  outcome at the next observation point rather than at return time — see
  "Deferred RESULT" below, it changes how you read them back. Node state
  (position, orientation, scale, visibility) is retained across commands;
  drawing it is still an explicit per-frame call. **These three are refused
  with `BUSY` while the autonomous loop is running** — see "Frame lifecycle"
  below for why, and use `DRAW_NODE`'s scene-graph siblings instead.
- `LOOP_START`/`LOOP_STOP`/`SCENE_COMMIT` ($06-$08) are implemented for
  **handshake mode** (`LOOP_START`'s `ARG0` = 0) and are **hardware-verified
  as of 2026-09-08** — a full `gpu64_loop_test` run (setup, 600 animation
  frames, teardown, `VERDICT PASS`) with the C64 alive at the end. The
  derails that made earlier builds unusable were an async DMA hold, not the
  protocol; see [project/progress_tracker.md](../project/progress_tracker.md)
  § 16b and § 16c. Two things that run teaches a calling program:
  **commit once per ready frame, not flat out** (poll `STATUS` bit4 —
  committing flat out is refused `BUSY` about 80% of the time, by
  construction, since the loop runs at the frame clock), and **check
  `SEQACK`**: at roughly 0.13 lost writes per accepted flip, a per-frame
  command is dropped every several frames, so re-issue on a mismatch rather
  than assuming it landed. Free-running mode (`ARG0` = 1, vsync-driven
  rather than `SCENE_COMMIT`-driven) answers `UNSUPPORTED` — staged in
  [project/gap_filling_plan.md](../project/gap_filling_plan.md).
- Scene-node and transform opcodes, **$20-$24 and $30-$36**, are **live**:
  a node created with `CREATE_OBJECT`/`CREATE_CAMERA` persists in a 256-node
  table until `DESTROY_NODE` or a session reset, and `DRAW_NODE` ($42) draws
  it using whatever the transform opcodes last set — no re-staging a
  position/orientation/scale on every draw the way `DRAW_MESH` needs. Every
  opcode in these ranges runs synchronously on core 0, but as of 15b each one
  first waits for any *immediate-mode* render it queued to finish draining on
  core 1 — it is about to mutate state that render also reads. **While the
  autonomous loop is running, these same opcodes instead write a shadow copy
  and return without waiting at all** — see "Frame lifecycle" below; the
  drain-before-mutate behaviour just described is what still happens when the
  loop is not running.
- `ARENA_STATUS` ($09) exists and was not in the original design sketch —
  see below.

In other words: **a class 1 program has two ways to drive a frame.**
Immediate mode calls `DRAW_MESH` directly (transform staged fresh every
call), or builds persistent nodes with `CREATE_OBJECT`/`CREATE_CAMERA` and
draws them explicitly with `DRAW_NODE` — every frame's draws are calls the
C64 makes itself, each one queuing on core 1 and returning without waiting
for its own draw to finish (something that actually needs the draw finished
— the next state-mutating opcode, a page flip, a class 0 framebuffer op —
pays that wait). The autonomous loop (`LOOP_START`/`SCENE_COMMIT`) instead
has core 1 render the whole scene every frame on its own: the C64's
per-frame work collapses to one small command per object that actually moved
this frame, then `SCENE_COMMIT` — a fire-and-forget trigger that publishes
those edits and returns immediately, never blocking on the render itself; it
finds out the next frame is ready by polling `STATUS` bit4. See "Frame
lifecycle" below for the full protocol.

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
| $06 | `LOOP_START` | 1 | `ARG0` 0 = handshake, 1 = free-running | Starts the autonomous render loop. `ARG0` must be 0 — free-running is `UNSUPPORTED`, staged next. `BUSY` if the loop is already running; `NO_CAMERA` if no camera is active at the moment of the call (checked once, here — not re-checked per frame after). Queues the loop's first frame and returns immediately; see "Frame lifecycle". |
| $07 | `LOOP_STOP` | 0 | — | Stops the loop. Always answers `OK`, even if no loop was running — a teardown call must not fail. A frame already queued when this arrives still finishes on core 1; it is discarded, not cancelled. |
| $08 | `SCENE_COMMIT` | 0 | — | Publishes every shadow-redirected edit made since `LOOP_START`/the last commit, arms the next vblank flip the same way `PAGE_FLIP` does, and queues the loop's next frame. `RESULT` = the page the just-finished frame is in — the same page the flip this call just armed is about to show. `UNSUPPORTED` if the loop is not running or the flip subsystem is not calibrated yet; `BUSY` if a `PAGE_FLIP` is already pending. See "Frame lifecycle". |
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
| $25 | `CREATE_SPRITE` | 2 | `ARG0-1` texture resource ID | Creates a billboard node under the staged `ID`. Starts one world unit square, masked and screen-upright, so a sprite you never call `SET_SPRITE` on is still a sprite you can see. Same replace-on-live-`ID` and `OUT_OF_MEMORY` behaviour as `CREATE_OBJECT`. |
| $26 | `CREATE_LIGHT` | 0 | — | Creates a point-light node under the staged `ID`, **off**: strength and radius both start at zero and it lights nothing until `SET_POINT_LIGHT` gives it both. Position it with the ordinary transform opcodes — it is a node like any other. |
| $27 | `SET_SPRITE` | 5 | `ARG0-1` width, `ARG2-3` height (8.8 world units), `ARG4` flags | `BAD_ID` unless the staged `ID` is a live sprite node. Flags: bit0 directional (`ARG0` of `CREATE_SPRITE` then names the **first of eight** consecutive texture IDs and the view drawn follows the camera, exactly as class 2's `DRAW_THINGS` does), bit1 depth-test but do not write, bit2 opaque (index 0 is a colour, not a hole), bit3 unlit. |
| $28 | `SET_POINT_LIGHT` | 3 | `ARG0` strength, `ARG1-2` radius (8.8 world units) | `BAD_ID` unless the staged `ID` is a live light node. Strength is in colormap levels added at the light's centre, falling off to nothing at `radius`. **Either being zero turns the light off** without destroying the node — which, with `SET_VISIBLE`, is the cheap way to switch one. |

### Transforms — $30-$3F

All act on the staged node `ID`. Every node type carries a
position/orientation, so these are how a camera is aimed, a sprite is put
somewhere and turned to face a direction (its yaw is what the directional
flag reads), and a light is moved — there is no separate "set light
position" opcode. `SET_SCALE` is the exception: it is used only by object
nodes, since a sprite is sized by `SET_SPRITE` and a light has no size.

Outside the loop these write into the retained node table, not a shadow copy
— an immediate-mode `DRAW_NODE` sees the change on its very next call. While
the loop is running they are redirected to the shadow scene and take effect
at the next `SCENE_COMMIT`; see Frame lifecycle.

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

Legal only with the loop stopped. While the autonomous loop runs, all three
answer `BUSY` — it owns the framebuffer and the scene for as long as it is
running.

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $40 | `CLEAR_VIEWPORT` | 0 | — | Fills the viewport with the background index and clears the z-buffer. Call this once per frame before drawing — nothing else clears the z-buffer for you (see Depth buffer below). |
| $41 | `DRAW_MESH` | 14 | `ARG0-5` position x,y,z (8.8), `ARG6-11` orientation, `ARG12-13` scale; mesh resource in `ID` | Transforms, lights, clips and rasterises one mesh into the draw page's viewport, z-tested. Queues on core 1 and returns immediately — see "Deferred RESULT" below for what that means for reading the outcome back. Ignores the scene graph entirely — draws in world space unless a camera has been applied by a preceding `DRAW_NODE` call this session (`SET_PERSPECTIVE`'s projection still applies either way). `RESULT` = triangle count drawn, saturated to a byte — **a mesh that draws 0 triangles and a winding-order mistake that culls every face look identical**, so check `RESULT` after your first upload of any new mesh. |
| $42 | `DRAW_NODE` | 0 | — | Draws the staged object node `ID` using its retained position/orientation/scale — no re-staging a transform, unlike `DRAW_MESH`. Queues on core 1 and returns immediately, same as `DRAW_MESH` — see "Deferred RESULT" below. Applies the active camera (if any) fresh on every call, so moving the camera between two `DRAW_NODE`s in one frame is seen by both — "every call" means every call as actually run on core 1, in the order queued, not the order a deferred `RESULT` is later read. `BAD_ID` if the staged `ID` is not a live object node (a camera `ID` included) — this much is still checked on core 0 before the call returns, since it can be. An invisible node (`SET_VISIBLE 0`) is skipped silently: `RESULT` = 0, `ERRCODE` = `OK`, same "0 is informative, not an error" convention `DRAW_MESH` uses for full culling. |

### Deferred RESULT

As of stage 15b (2026-08-29), `CLEAR_VIEWPORT`/`DRAW_MESH`/`DRAW_NODE` queue
onto core 1 and return without waiting for their own draw to finish — that
overlap, not a stall on every one, is the point: a frame of `DRAW_NODE` calls
now queues and runs while the C64 goes on to its next command instead of
halting for each one in turn. The trade is that `ERRCODE` on return only
reflects what core 0 could check before queuing (a bad `ID`, no active
camera, an argument out of range) — never the draw's own outcome — and
`RESULT` is **not** updated to that draw's triangle count at return time.

`RESULT` is valid **as of the last observation point** instead: it catches up
to the most recently queued `DRAW_MESH`/`DRAW_NODE` the next time anything
forces the ring to finish draining, which happens automatically and often —
every other class 1 opcode does it before it runs (they mutate state a queued
draw also reads), and so does a `PAGE_FLIP`/`VBLANK_SYNC` and any class 0
opcode that touches the framebuffer. In practice this means: **the debugging
workflow "check `RESULT` after your first upload of any new mesh" still
works exactly as written** — issue the `DRAW_MESH`, then issue essentially
any other command (a `SET_*`, a class 0 `CLEAR`, a `PAGE_FLIP`) and read
`RESULT` — no dedicated "wait" opcode is needed because ordinary programs
already call something else next. Two things to not use for this: another
`DRAW_MESH`/`DRAW_NODE` does **not** reliably flush the previous one's
`RESULT` — the render ops only drain when the ring is full, not on every
call, so back-to-back draws can leave the first one's `RESULT` unread for
longer than one call; and an opcode that defines its own `RESULT`
(`ARENA_STATUS`, `UPLOAD_MESH`) overwrites the just-flushed value with its
own the instant it runs, so it never actually surfaces the draw's triangle
count even though it does force the wait. A program with truly nothing else
to issue should use `NOP` ($00, class 0) instead: it is genuinely
side-effect-free and forces the same wait — see `gpu64_3dSync()`'s comment
in `gpu64_3d.h`.

A queued-but-not-yet-drained ring is also why `QUEUE_FULL` is reachable for
`CLEAR_VIEWPORT`/`DRAW_MESH`/`DRAW_NODE` far less often than it used to be:
pushing against a full ring waits (bus held) for core 1 to make room rather
than rejecting outright, with `WORKER_TIMEOUT` as the backstop against a
wedged core 1 — see the `ERRCODE` table below. A genuinely full ring only
still rejects outright for the other class 1 opcodes, which is the design's
"a failed dispatch does nothing" rule doing its job on a command with no
useful way to wait.

## Frame lifecycle

The autonomous loop (handshake mode, `LOOP_START ARG0=0`, live as of
stage 16) is the model the whole class 1 design has been building toward:
core 1 renders a whole frame on its own, every commit, instead of the C64
issuing each draw itself. The frame is, in order:

1. clear the viewport (colour and z-buffer),
2. apply the active camera,
3. apply the live point lights,
4. every visible `OBJECT` node, in scene order,
5. every visible `SPRITE` node, in scene order.

Objects before sprites, and not one interleaved pass, because sprites are
depth-tested against geometry that is by then already in the z-buffer. A
node whose mesh or texture ID does not resolve is skipped, not an error —
one stale ID must not blank an otherwise-good frame.

**Starting it.** `LOOP_START` snapshots the live scene/render state into a
shadow copy and queues the first frame. From that point on, every opcode
that would normally mutate the live scene graph or per-frame render state
(`SET_VIEWPORT`/`SET_PERSPECTIVE`/`SET_LIGHT`/`BUILD_COLORMAP`/
`SET_BACKGROUND`, every $20-$28/$30-$36 scene-node opcode) instead writes the
**shadow** copy and returns immediately, without waiting on the ring at
all — that's what makes moving ten objects between two commits cheap: none
of those ten calls can race the frame core 1 is currently rendering, because
neither one touches what the other reads. `CLEAR_VIEWPORT`/`DRAW_MESH`/
`DRAW_NODE` (immediate mode) are refused with `BUSY` for as long as the loop
runs — they read/write the live scene/framebuffer directly, exactly what the
loop's own render is doing on core 1 at the same time.

**Each frame.** Move whatever needs to move with the ordinary scene-node
opcodes, then call `SCENE_COMMIT`. It publishes every shadow edit since the
last commit into the live scene (what the *next* frame renders from), arms
a page flip the same way `PAGE_FLIP` does (so the *just-finished* frame
actually reaches the screen), and queues the next frame on core 1 — all
without the C64 waiting for that next frame to actually finish rendering.
`RESULT` after `SCENE_COMMIT` returns is the page the frame just flipped to
was rendered into.

**Polling for the next one — wait on two bits, not one.** The predicate a
frame loop must poll is `FRAME_READY` **set and `BUSY` clear**:

```asm
    lda STATUS
    and #$11                ; FRAME_READY | BUSY
    cmp #$10
    bne poll                ; not ready, or a flip is still pending
```

`STATUS` bit4 (`GPU64_STATUS_FRAME_READY`) goes high once core 1 has finished
a frame. Both bits are updated on the **frame clock**, inside the same vblank
handler that retires the flip — not only when the C64 happens to send a
command — so a read-only poll loop is enough and no command traffic is needed
to make progress. (Before 2026-09-08 the bit really was raised only on a
dispatch, which made this loop stall for its whole timeout on any frame where
core 1 finished after the last command of the frame; if you are running older
firmware, that is the symptom.) Bit4 says nothing about the page flip the
*previous*
`SCENE_COMMIT` armed: that flip only retires at the next vblank, and until it
does, `SCENE_COMMIT` answers `BUSY` rather than arming a second flip over an
un-presented one. Bit0 (`GPU64_STATUS_BUSY`) is cleared by the same vsync
handler that clears the pending flip, so the two-bit test is exactly the
question "may I commit now?".

Polling `FRAME_READY` alone is not incorrect — it never commits against a
still-in-flight render — it is just wasteful: the loop spins issuing commits
that are refused, and each refusal costs a DMA hold that stops the 6510. At
the bench on 2026-09-08 the Quake demo drew 6099 accepted frames against
15838 `BUSY` answers, 72% of its commit traffic thrown away, while still
rendering at the full 60 Hz the flip rate allows.

**Stopping it.** `LOOP_STOP` always succeeds. A frame already queued when it
arrives still finishes on core 1 — there is no way to cancel a frame in
flight — but nothing further reads its result once the loop is stopped.
`SCENE_RESET` also stops the loop (and drops the shadow copy) as part of
destroying every node, per its own row above.

**No 2D overlay while the loop runs.** The loop owns the framebuffer, and
in handshake mode there is no moment between its frames at which the C64 may
draw into the page that is about to be shown: a class 0 framebuffer op
issued against a running loop is racing core 1's render of that same page.
So a HUD, a weapon sprite, a status bar — anything that used to be drawn
over a finished 3D view with class 0 — cannot be done this way. Either put
it in the scene (a `SPRITE` node with the unlit flag, positioned in front of
the camera) or keep it on the C64's own screen, which is where the class-1
Quake demo puts its report. This is a real limitation of the stage-16 loop,
not an oversight; immediate mode (`DRAW_MESH`/`DRAW_NODE` with the loop
stopped) has no such restriction and can still be overlaid freely.

**What free-running mode (`LOOP_START ARG0=1`) will add**, once built: the
same loop driven by vblank instead of by `SCENE_COMMIT` — the C64 never
calls anything per frame at all, it just edits the shadow state whenever it
likes and the loop picks up whatever was last written each vsync. Not yet
implemented; `ARG0=1` is `UNSUPPORTED`.

## What retaining the scene actually costs

`Source/Demos/gpu64_demo_quake.a` and `Source/Demos/gpu64_demo_quake3d.a`
are the same room, the same monsters and the same controls, written twice:
once in class 2's immediate mode, once as a retained class 1 scene driven
by the handshake loop. The second is the port stage 17 asked for, and it
exists so that "retained is cheaper on the bus" is a measurement rather
than an argument.

The figures below come from `tools/prgsim`, which counts every dispatch and
every write into the `$DF0B-$DF23` window. They are **marginal** per-frame
costs — a 400-frame run subtracted from an 800-frame one — so one-time
start-up traffic is out of them. That start-up is smaller than it sounds:
about 60 dispatches and 570 register writes for class 1 (against class 2's
45 and 450) to create every node and upload the mesh, ten textures and the
colormap, because bulk data moves by REU DMA — the C64 writes a blob
descriptor, never the bytes. "Moving" holds two keys down so the camera
changes on every frame; "standing still" touches nothing.

| Per frame | class 2, immediate | class 1, retained |
|---|---|---|
| moving — dispatches | 11 | 6 |
| moving — register writes | 102 | 70 |
| standing still — dispatches | 11 | 4 |
| standing still — register writes | 102 | 42 |

Class 2 pays the same on every frame because it redraws: camera, two
lights, `FILL_VIEW`, `DRAW_WORLD`, `DRAW_THINGS`, `STATS`, the weapon
sprite, the status bar and the flip go out whether or not anything moved.
Class 1's six are the camera's position and orientation, the orientation of
the monster that turns, the position and orientation of the monster that
walks, and `SCENE_COMMIT`. Everything else — thirty triangles, ten
textures, the directional light, the torch, and every transform that did
not change — is resident and is never sent again.

**Read the table honestly in two places.**

*It is not a like-for-like comparison of 3D work.* Part of class 2's
per-frame total is class 0: the status bar and HUD text it draws onto the
HDMI view, which class 1 cannot do at all while the loop owns the
framebuffer (see above). Split by class, class 2's frame is **7 dispatches
and 74 register writes of class 2 rendering, plus 4 and 28 of class 0
overlay**; class 1's is class 1 throughout. So against the rendering half
alone the retained scene is only slightly ahead while the player is moving
(6 / 70 against 7 / 74) and properly ahead when he is not (4 / 42) — the
headline 11 → 6 is in large part the HUD moving to the C64's own screen
rather than traffic that vanished. Say it that way round.

*The number that matters most is not in the table.* The C64 is DMA-halted
for the whole of every dispatch, and only for a dispatch: `waitReady`'s
poll of `STATUS` bit 4 is a plain read that costs nothing. So the dispatch
column is also the count of times per frame the C64 stops dead — and the
frame those six commands describe is rendered on core 1 while the C64 keeps
running, which is the part immediate mode cannot do at any price.

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

### Point lights

On top of the directional light, up to **eight** point lights, each a
`CREATE_LIGHT` node placed by the ordinary transform opcodes and given a
strength and a radius by `SET_POINT_LIGHT`. Their contribution is added to
the level the directional light produced, so a scene with a low `ambient`
and no directional light at all is lit only where the point lights reach.

- Meshes are lit **per pixel**: the view-space position is interpolated
  across the triangle the same affine way `u`/`v` are. A wall crossing a
  light's radius gets a gradient, not one flat step.
- Sprites are lit **per record**, once at the billboard's own position —
  a billboard has no interior geometry for a gradient to live on.
- The cap is eight *live* lights: a light node is skipped when it is hidden
  (`SET_VISIBLE 0`) or inert (strength or radius zero), so a scene may hold
  many more than eight as long as no more than eight are lit at once. Past
  eight, the **first eight in scene order** win — scene order is node-table
  slot order, which is the order the IDs were created in.
- The whole lit path sits behind one branch outside the pixel loop, so a
  frame with no live lights costs exactly what it did before lights existed.

Switching a light is therefore `SET_VISIBLE`, not create/destroy: a muzzle
flash is one node created at start-up, moved to the player each frame, and
made visible for as long as the shot lasts.

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
| $DF21 | `RESULT` | R | Low byte of the last command's result — page number from `SCENE_COMMIT`, triangle count from `DRAW_MESH`/`DRAW_NODE`, face count from `UPLOAD_MESH`. Meaning is per-opcode; undefined for opcodes that define none. `CREATE_OBJECT`/`CREATE_CAMERA` don't allocate an ID and so don't set `RESULT` — node IDs are chosen by the C64 side, same as resource IDs. As of stage 15b, `DRAW_MESH`/`DRAW_NODE`'s `RESULT` is deferred — see "Deferred RESULT" above for when it actually becomes valid. |

and one `STATUS` bit:

| Bit | Meaning |
|---|---|
| 4 | frame-ready — the loop has finished a frame and is waiting for `SCENE_COMMIT` (handshake mode only). Polled at the top of every dispatch, so it goes high as soon as core 1 finishes, not only when `SCENE_COMMIT` itself runs. |

New `ERRCODE` values this class adds: `OUT_OF_MEMORY` (resource RAM
exhausted), `QUEUE_FULL` (the ring was full and the opcode was one of the
ones that still rejects outright rather than waiting — see "Deferred RESULT"
above), `BAD_ID` (no such resource or node), `NO_CAMERA` (a render was asked
for with no active camera), `WORKER_TIMEOUT` (core 0 gave up waiting on core
1 in a generous worst-case window — designed to be unreachable in normal
operation). As of stage 15b this has two distinct triggers, not one: a
`CLEAR_VIEWPORT`/`DRAW_MESH`/`DRAW_NODE` pushed against a full ring waiting
for space to open up, or any other class 1 opcode (including the drain
inside a session reset, i.e. RUN/STOP+RESTORE) waiting for a previously
queued render to finish before it mutates the state that render reads. Both
mean the same thing at the hardware level — core 1 did not drain the ring in
time — just reached from two different call sites. See
[error-codes.md](error-codes.md) for the shared table.
