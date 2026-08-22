# gpu64 command API design

Living document for milestone 4 (see [progress_tracker.md](progress_tracker.md)).
Captures decisions as they're made, and the open questions still blocking them.
Companion to [project_description.md](project_description.md) (IO address space
allocation, target hardware configs) and [bus_access_design.md](bus_access_design.md)
(why cycle-by-cycle sniffing doesn't fit this hardware).

## Philosophy

- **IO2 carries commands only, never bulk payload.** A command that needs data
  (a texture, a mesh's vertex buffer) passes a *reference* — where the data
  already lives (C64 RAM or REU memory) and how long it is — not the data
  itself byte-by-byte through a register. gpu64 pulls the referenced bytes via
  DMA burst, the same cycle-stealing technique milestone 3's mirror snapshot
  already proved on hardware, just parameterized instead of hardcoded to
  screen RAM.
- **Resources live and stay GPU-side.** A texture, once uploaded, is identified
  by an ID the GPU owns; it does not need to keep occupying C64 RAM, and later
  rendering commands reference it by ID with no further DMA. Same for meshes,
  once mesh upload is designed.
- **The GPU runs a continuous, independent render loop.** C64-side API calls
  set up the scene (meshes, textures, cameras, lights) and then update it in
  real time; the actual per-frame rendering and HDMI scanout run on the RPi
  on their own schedule, decoupled from whatever the C64 is doing. The C64
  is not blocked waiting for frames to render — see the architecture section
  below for what this requires.

## Address space

Settled in [project_description.md](project_description.md#io-address-space-allocation):
gpu64 owns **$DF0B–$DFFF, 245 bytes** of IO2. IO1 ($DE00–$DEFF) stays
reserved/untouched (needed for target config #3, a real hardware REU sharing
the bus). REU keeps $DF00–$DF0A.

Implementation note: the *current* `reuUsingPolling()` dispatch masks
`IO_ADDRESS & 0x1f` — that's REU's own internal decode (11 registers fit in 5
bits), not a hardware limit; the GPIO latch delivers the full 8-bit address.
gpu64's own dispatch must decode the full byte, not inherit that mask, or the
usable space silently shrinks to ~21 aliased addresses.

## Memory model

Three distinct spaces, not two:

- **C64 RAM** — 64KB, small and transient.
- **REU** — up to 16MB, independent C64-side expansion memory (not GPU
  storage; just a bigger source/destination than C64 RAM, addressed the same
  way). gpu64 does not treat REU contents as already-resident — pulling from
  it costs a real DMA burst, same as pulling from C64 RAM.
- **GPU VRAM** — RPi RAM, ~1GB. Where uploaded resources actually live
  (textures, meshes, world data). Always the implicit "other end" of an
  upload/download command; never named as a `SRC_SPACE`/`DST_SPACE` value
  itself since it's the fixed destination (upload) or source (download).

Uploads go C64-RAM-or-REU → VRAM; downloads go VRAM → C64-RAM-or-REU. Which
direction a given command uses is implicit in the opcode (e.g.
`uploadTexture` vs. `downloadFramebuffer`), not a separate flag register.

## Register map (draft, $DF0B–$DFFF)

| Offset | Name | Dir | Purpose |
|---|---|---|---|
| $DF0B | `CMD_A` | W | Opcode, class 0 (2D + math/matrix + system); write triggers dispatch once `ID`/`ARG` are staged |
| $DF0C | `CMD_B` | W | Opcode, class 1 (3D); write triggers dispatch once `ID`/`ARG` are staged |
| $DF0D | `STATUS` | R | bit0 busy, bit1 error, bit2 vblank-pending, bit3 vblank-IRQ-armed, rest TBD |
| $DF0E | `ERRCODE` | R | Set on error (e.g. `QUEUE_FULL`); cleared on next successful `CMD_LO` |
| $DF0F–$DF10 | `ID` | W | Resource ID (texture/mesh/etc.), 16-bit |
| $DF11–$DF20 | `ARG0`–`ARG15` | W | Generic 16-byte argument block, layout defined per-command (see below) |
| $DF21–$DFFF | — | — | Reserved for growth |

### ARG block convention for DMA-referencing commands

Not a fixed register layout — each command defines its own use of the 16
`ARG` bytes — but commands that reference a DMA payload (upload or download)
use a shared 6-byte **blob descriptor** shape within it:

| Bytes | Field | Purpose |
|---|---|---|
| 1 | space | 0 = C64 RAM, 1 = REU |
| 3 | addr | 24-bit offset within that space (covers REU's full 16MB; C64 RAM just uses the low 16 bits) |
| 2 | len | byte count |

- **Single-blob command** (e.g. `uploadTexture`): one descriptor (6 bytes),
  10 bytes left for command-specific fields (width, height, format, ...).
- **Two-blob command** (e.g. mesh upload: vertex array + index array): two
  descriptors (12 bytes), 4 left over.
- **N-blob command** (N > 2): the descriptor in `ARG` points not at the
  payload itself but at a small table of further descriptors sitting in
  C64 RAM/REU; gpu64 DMA-reads that table first, then pulls each blob in
  turn. Scales to arbitrary blob counts without growing the register window.

Download commands use the same descriptor shape with reversed semantics
(gpu64 writes to `space`/`addr`/`len` instead of reading from it).

## Command dispatch model

**Protocol**: stage every other register first — `ID`, `ARG` bytes, in any
order, any number of writes — then a single `STA` to `CMD_A` (2D/math/
system) or `CMD_B` (3D) fires dispatch using whatever is currently staged.
This is the same shape REU's own protocol already uses in this firmware
(stage address/length, then write `COMMAND` with the execute bit) — proven,
not new.

Two independent one-byte trigger registers, one per class, rather than a
trigger register plus a sticky class selector: deliberately avoids the
`gpu64ApiActive`-shaped bug class (state set once, cleared nowhere, silently
wrong for the rest of the session — see [progress_tracker.md](progress_tracker.md#3-display-sniffed-current-c64-screen-buffer))
and means interleaving 2D/math and 3D commands costs no extra write either
way — always exactly one `STA`, to the address that already says which
class you meant. Future classes (WiFi, etc.) get their own `CMD_C`/`CMD_D`/
... trigger byte on the same pattern rather than a shared selector.

On the `CMD_A`/`CMD_B` write, core 0 (the existing cycle-accurate bus watcher):

1. If the command references a DMA payload: performs the burst(s) itself,
   synchronously, inside the write handler — same mechanism as
   `gpu64_mirrorSnapshot()`. Cost is proportional to the descriptor's `len`,
   same as REU's own Store/Fetch already is — predictable, not free, and
   that's fine (see Cycle-predictability below).
2. Copies the resulting payload (or the inline `ARG` bytes, for DMA-less
   commands) into a queue slot, and pushes a lightweight descriptor
   (opcode + ID + pointer/length) onto a ring buffer.
3. Returns — the C64 write cycle completes immediately. It is safe for the
   C64 to reuse/overwrite its source RAM as soon as the `STA` retires,
   because the pull already happened synchronously in step 1. No busy-wait
   needed for uploads, and it is safe to immediately start staging the next
   command's registers — there's no race, since the 6502 doesn't get the
   cycle back until the pull is done.

Core 1 (the render loop, see architecture below) drains the ring buffer at
its own pace and applies each command to GPU-side state.

This means most commands are fire-and-forget from the C64's perspective —
`STATUS.busy` is not needed for the common case. It's reserved for the rare
op that must block (TBD which, if any, turn out to need it).

### Cycle-predictability

Core 0's write handler must never do anything with *unbounded* or
*contention-dependent* latency — no allocation, no blocking locks, no
unbounded cache work. Two consequences:

1. A DMA pull's cost scales with payload length, but that's a known,
   C64-computable cost, not the kind of unpredictability we're avoiding.
2. **The core-0→core-1 ring buffer must never block core 0.** If core 1
   falls behind and the buffer is full, core 0 rejects the command
   (`STATUS.error` + `ERRCODE = QUEUE_FULL`) rather than waiting. Should be
   rare in practice (a render loop should drain far faster than commands
   arrive) but must stay deterministic regardless.

## Architecture: this needs two cores

**This is the open question that gates everything else in this document,**
and should be the first thing prototyped, before the rest of the opcode/
register design is locked in.

Today, `reuUsingPolling()` is a single tight busy loop: it asserts
`bDMA_OUT` once (`rad_reu.cpp:392`) and never releases it for the life of
the REU session, bit-banging every PHI2 half-cycle to keep up with REU's and
gpu64's register access. That loop is fully occupied doing this — there is
no spare time in it to also run a continuous 3D rendering loop.

To get "C64 free-runs, GPU renders independently and continuously" (this
doc's stated goal), the plan is:

- **Core 0**: keeps doing exactly what it does today — the proven,
  cycle-accurate REU/gpu64 register watch loop, untouched. Adds: full 8-bit
  IO_ADDRESS decode (see above), the DMA-by-reference pull described above,
  and pushing descriptors onto a ring buffer.
- **Core 1**: owns all GPU-side state (resource table, scene graph,
  framebuffer/back-buffer), drains the ring buffer, runs the render loop,
  drives HDMI scanout, and asserts the vblank IRQ (see below).
- Communication: a lock-free single-producer/single-consumer ring buffer in
  shared RAM. Needs real attention to Cortex-A53 cache coherency/memory
  barriers between cores — Circle's multicore support (`CMultiCoreSupport`)
  needs to be evaluated for whether it fits a bare-metal, cycle-critical
  core 0 without disturbing its timing.

**Unverified — first thing to prototype on hardware:** whether Circle's
multicore startup and a second core doing normal (non-cycle-critical) work
can coexist with core 0's cycle-exact bit-banging without jitter. If this
doesn't pan out, the fallback is a more modest goal (bounded per-frame
command batches processed between C64-visible bus windows, still on one
core) — worth deciding as a fallback plan, not discovering by surprise.

Decided: use as many cores as needed (RPi 3A+ has four) — not limited to a
strict 2-core split. A future independent WiFi API is planned too, which
will likely want its own core/loop eventually; the `CMD_CLASS` reservation
above leaves room for it at the protocol level. Verify the core split on
real hardware sooner rather than later, before more of the API is designed
on top of an unverified assumption — a minimal skeleton (core 0 unchanged,
a second core just confirmed alive/scheduled) is the next concrete step.

## vblank signaling

Confirmed available and already proven on this hardware: `IRQ_OUT`/`bIRQ_OUT`
is a real GPIO line into the CBTD3861 transceiver, and REU's own emulation
already drives it (`rad_reu.cpp:605,618`) for REU's completion-interrupt
feature. gpu64 can reuse the same mechanism for vblank:

- `STATUS.vblank-pending` bit for simple polling (BASIC-friendly, no IRQ
  handler needed).
- An IRQ pulse on vblank, gated by a C64-settable "arm" bit
  (`STATUS.vblank-IRQ-armed` / a small `ARG`-based enable command), for
  programs that want a real interrupt instead of polling.

**Decided: stays armed, auto-rearms.** Like a raster IRQ, the handler
acknowledges on entry; re-arming automatically for the next frame avoids
needing an explicit re-arm write every single frame, and matches how raster
IRQs are normally used on the C64. A C64-settable disarm (to go back to
polling, or stop the interrupts entirely) stays available.

## Opcode space

Which trigger register you write selects the class (see register map
above): `CMD_A` = 2D + math/matrix + system, `CMD_B` = 3D, future classes
get their own trigger byte. 256 opcodes per class. Minimum viable set
carried over from the progress tracker, now sorted into classes:

- **Class 0 — 2D + math + system**: clear/fill, set pixel, line, rect
  (filled/outline), blit-by-reference, palette select, page flip; vector/
  matrix ops (add, multiply, transform, inverse, ...); `freeResource(id)`,
  status/error queries.
- **Class 1 — 3D**: `uploadTexture(id, ...)`, mesh upload (vertex/index
  blobs, see ARG block convention above), camera setup, light setup,
  per-frame scene/transform updates, `downloadFramebuffer(...)`.

Math/matrix ops live in class 0 (`CMD_A`) rather than getting their own
class — they're likely to be used every frame (camera/transform updates),
so they belong alongside 2D on the register C64 code will write to most.

To fill in next: full opcode table with per-opcode `ARG` byte layout, once
the register map above is validated against a couple of concrete commands
(texture upload and one scene-update command are enough to stress-test the
register widths).

## Resource lifecycle

Uploading `uploadTexture(id=5, ...)` allocates a chunk of VRAM and files it
under ID 5.

**Decided:**
- **Re-upload to a live ID implicitly frees the old allocation and
  replaces it** — convenient for "reload this texture" without a
  free-then-upload dance.
- **Explicit `freeResource(id)` only, no automatic eviction.** VRAM is
  ~1GB against a C64/REU-side source data ceiling of 16MB, so running out
  is expected to be a hard error in practice, not a real constraint. This
  also keeps VRAM state deterministic, in the same spirit as the
  cycle-predictability goal above (different layer, same principle).
- **One flat 16-bit ID namespace** across all resource types (textures,
  meshes, future resource kinds) — `ID` is already 16 bits wide, so no
  reason to split it per type.

## Open questions

1. Multicore feasibility (see Architecture above) — blocks everything else,
   prototype first.
2. Mesh upload's exact `ARG` layout (stride, attribute layout, interleaved
   vs. separate arrays) — bespoke per your last note, design when we get
   there.
3. Error handling: `ERRCODE` values not enumerated yet — needs the opcode
   list first.

## To be continued...
