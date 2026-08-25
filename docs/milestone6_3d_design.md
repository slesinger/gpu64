# gpu64 3D layer design (milestone 6)

Design notes for the 3D API layer and the continuous, independent render
loop it needs — split out from [api_design.md](api_design.md) (milestone
4's 2D/math command protocol, which this document builds on but does not
duplicate). Status/history live in
[progress_tracker.md](progress_tracker.md); this file is API/architecture
design only.

Nothing here is required for milestone 4. It's recorded now because the
target end-state — C64 sets up a scene once, then the GPU renders it
continuously and independently while the C64 goes on to do something else
entirely (per the user's own framing: play BASIC on the native VIC-II
display while the HDMI-side scene keeps rendering) — genuinely needs an
architecture milestone 4 doesn't, and it's worth having settled before
milestone 6 implementation starts rather than discovered mid-build.

## Binding constraint: the bus drops writes

Before designing anything that streams data C64-side to GPU-side, know that
**an IO2 register write is occasionally not sampled** — roughly 1 in 35000 REU
transfers and 1 in 180000 gpu64 command writes, with no retry and no error.
This is pre-existing and reproduces with `GPU64_3D_ENABLED` off. See
*IO2 write sampling* in [progress_tracker.md](progress_tracker.md) (handle
`io2-sampling`) for the full characterisation.

Three consequences bind on everything below:

- **Commands need a sequence number** with a firmware-side gap detector, so a
  dropped argument write is reported rather than executed against a
  half-written argument block.
- **Mesh and texture upload through REU DMA needs a checksum or a length
  readback.** That path carries a second, larger and still unattributed defect
  that latches to 100% failure under REU-register write pressure.
- **Do not add per-write work to either IO2 decode path in `rad_reu.cpp`.** The
  REU window already resets `reu.pl`/`reu.pl2` and calls `reuPrefetch()` on
  every write; the gpu64 window deliberately does neither, and that difference
  is why command traffic has never reproduced the latch.

## Philosophy

- **Resources live and stay GPU-side.** A texture or mesh, once uploaded,
  is identified by an ID the GPU owns; it does not need to keep occupying
  C64 RAM, and later rendering commands reference it by ID with no further
  DMA.
- **The GPU runs a continuous, independent render loop.** C64-side API
  calls set up the scene (meshes, textures, cameras, lights) and then
  update it in real time; the actual per-frame rendering and HDMI scanout
  run on the RPi on their own schedule, decoupled from whatever the C64 is
  doing. The C64 is not blocked waiting for frames to render.

This does **not** mean the C64 is ever DMA-halted for longer than a single
bounded burst — see [project_description.md](project_description.md#operating-modes).
"Independent render loop" describes where continuous rendering work runs
(a separate core, on its own schedule), not a change to how the C64's bus
is serviced.

## Memory model addition: GPU-side resource RAM

Milestone 4 only ever DMAs into the existing framebuffer. Milestone 6 adds
a third space:

- **GPU-side resource RAM** — plain RPi system RAM (not dedicated video
  memory; the RPi 3A+ has no such thing) used to hold uploaded resources —
  textures, meshes, world data — independently of the framebuffer itself.
  512MB total on the RPi 3A+ (corrected from an earlier ~1GB estimate,
  which was the 3B+'s figure), minus Circle and up to 16MB of `reuMemory`.
  Always the implicit destination of an upload / source of a download;
  never itself a `space` value in a blob descriptor (see api_design.md) —
  only C64 RAM and REU are addressable sources/destinations.

Uploads go C64-RAM-or-REU → resource RAM; downloads go resource RAM →
C64-RAM-or-REU. Direction is implicit in the opcode (`uploadTexture` vs.
`downloadFramebuffer`), not a flag register.

## Architecture: a second core for the render loop

Per the philosophy above, this needs somewhere for continuous rendering
work to run that isn't core 0's bus-watch loop. The rest of this section is
**unproven** and is the first thing to prototype before more of milestone
6 is designed on top of it.

- **Core 0**: unchanged from milestone 4 — the cycle-accurate REU/gpu64
  register watch loop. Adds: performing a command's DMA pull synchronously
  (as in api_design.md), then pushing a lightweight descriptor
  (opcode + ID + pointer/length) onto a ring buffer instead of acting on it
  directly.
- **Core 1**: owns all GPU-side state (resource table, scene graph,
  framebuffer/back-buffer), drains the ring buffer, runs the render loop,
  drives HDMI scanout, asserts the vblank IRQ.
- **The core-0→core-1 ring buffer must never block core 0.** If core 1
  falls behind and the buffer is full, core 0 rejects the command
  (`STATUS.error` + `ERRCODE = QUEUE_FULL`) rather than waiting — core 0's
  cycle-predictability requirement (milestone4_2d_api_design.md)
  applies here too.

### The real risk: store bursts, not core-count and not the L2

**Settled 2026-08-23 after eighteen rounds of the load ladder. This
supersedes every earlier amendment in this section; the narrative below is
kept only as the record of how it was found.**

Three shared resources were suspected in turn. Two of them are real
constraints, and the third -- the shared L2 -- turned out not to be the
axis at all.

**1. The peripheral bus is contended, and it is the sharpest constraint.**
Core 0's timing rests on tight MMIO polling of `ARM_GPIO_GPLEV0`, so it
depends on the *latency* of every one of those reads. The ladder's first
round proved this by accident: core 1 spinning on the BCM system timer, an
MMIO register on the same bus, corrupted gpu64 command arguments while
writing zero bytes of DRAM traffic. **The render loop must not poll any MMIO
register in a spin.** Use `CNTVCT_EL0` for time -- it is core-local, costs
no bus cycle, and spinning on it at a 64us cadence was measured completely
free (round 14).

**2. Core 0 must not read any line another core writes.** Separating the
lines is not enough on its own -- the discovery in round 8 was a mailbox
sharing a line with core-0 state, but the fix that actually took the noise
floor to zero (round 10) was removing core 0's *read* of the worker's line
entirely. A coherence miss is ~100 cycles, but the polling loop is
PHI-locked, so missing a deadline by 100 cycles costs a whole 710-cycle VIC
half-cycle. Every core-0/core-1 mailbox must be one-way, 64 bytes, and
64-byte aligned; see `Gpu64LadderToWorker` in gpu64_ladder.h for the pattern.

**3. The real memory constraint is the length of an unbroken run of stores
-- not bandwidth, not working-set size, not cache residency in the general
case.** A run of consecutive stores occupies the memory system for roughly
its own duration, and a PHI-locked loop pass has about **1us of slack**.
Anything that fits in that slack is free; anything that does not derails the
C64. The measured numbers, all at 2 million loop passes per rung:

| what core 1 does | verdict |
|---|---|
| burst 5 lines (320 B), cache-resident, at **64 MB/s** | clean (round 17) |
| burst 8 lines (512 B), cache-resident, at **25 MB/s** | clean (round 17) |
| burst 12 lines (768 B), cache-resident | marginal -- 2 late passes per 2M |
| burst 16 lines (1 KB), cache-resident | **fatal**, derailed at 471k passes |
| burst 4 lines (256 B), **DRAM-resident** (cold, every store misses), at 6.4 MB/s | clean (round 18) |
| burst 7 lines (448 B), **DRAM-resident**, at **11.2 MB/s** | clean (round 18) |
| burst 8 lines (512 B), **DRAM-resident**, at only 1.6 MB/s | **fatal**, derailed at 1762k passes (round 17) |
| burst 8 lines via `DC ZVA`, **DRAM-resident** | **fatal in 18k passes** -- an order of magnitude worse than plain stores (round 18) |

**There is no rate ceiling.** Burst 5 at 64 MB/s is indistinguishable from
burst 5 at 1 MB/s -- same worst-case pass, zero losses. That is twenty times
what a 320x200 50 fps double-buffered renderer needs, for free. Two full
orders of magnitude of rate change nothing; one step up in burst length
kills the machine. Rate and working-set size are both non-axes.

**Cache residency is not an axis in itself; it shortens the burst slightly.**
A cold store must allocate its line (read-for-ownership from DRAM) before it
retires, so a cold line occupies the memory system longer than a warm one --
but the effect on the threshold is small: **8 lines warm, 7 lines cold.** The
limit sits at roughly half a kilobyte of unbroken stores either way, which is
about what ~1 us of slack buys. Round 17 briefly suggested a much larger gap;
round 18 closed it by testing the cold side properly.

**The design rule, as it stands:**

> The render core may write **at most 7 consecutive cache lines (448 bytes)
> before yielding** -- 8 if the target is cache-resident. Below that,
> throughput is unconstrained to at least 11 MB/s cold and 64 MB/s warm, and
> the working set may be any size.
>
> **Design to 256-byte spans (4 lines).** That is 57% of the measured edge,
> was clean at both 800 KB/s and 6.4 MB/s, and maps onto a natural unit: a
> 320-pixel 8bpp scanline is 320 bytes, so one yield per scanline fits with
> room to spare.

**There is no way to buy a longer burst.** Both mitigations were tried on
hardware and both failed: `STNP` (round 6 -- the A53 treats the non-temporal
hint as a no-op) and `DC ZVA` (round 18 -- it writes a line with no
read-for-ownership, which should have helped, and instead derailed the machine
a hundred times faster than plain stores, most likely because eight `DC ZVA`
instructions pack eight line allocations into a handful of cycles where
sixty-four ordinary stores spread them out). The render loop has to yield.

What this means for the render design:

- **Tile-based rendering is no longer required.** The earlier conclusion
  that a full-size DRAM framebuffer "does not work at any frame rate" was
  wrong; it came from a synthetic rung that wrote thousands of lines without
  yielding. A DRAM framebuffer is fine provided the rasteriser breaks its
  writes into short spans.
- **Span length is now a first-class design parameter**, and 256 bytes is
  the safe unit: a 320-pixel scanline at 8bpp is 320 bytes, so the natural
  yield point is per-scanline, or twice per scanline.
- **The 512 KB L2 budget is not a budget.** A working set larger than L2
  costs nothing by itself; only the burst length does.

**A caveat on all of the above.** The ladder's metric samples the loop
period at the top of the loop, which is a resynchronisation point, so a
stall that lands *mid-pass* corrupts the bus transaction while the period
still reads healthy. Both fatal rungs above scored zero late passes right up
until the C64 died. **A clean metric is necessary, not sufficient** -- the
C64's own behaviour is the ground truth, and any future render loop needs a
real REU round-trip running underneath it as the positive control.

See progress_tracker.md milestone 6a, rounds 1-18, for the full record --
including three published diagnoses (false sharing, DRAM line fills, write
rate) that later rounds killed.

#### How it was found (historical; superseded by the above)


A first design pass here assumed the only things to verify were (a) whether
Circle's multicore startup coexists with core 0's bit-banging at all, and
(b) cache-coherency/memory-barrier correctness between cores. Both matter,
but neither is the main risk. The Cortex-A53's L2 cache is **shared across
all four cores**, and core 0's entire timing strategy is a hand-tuned L2
preload schedule (a 0x1a00-byte instruction window walked every cycle, plus
per-transfer cache warming in `handle_transfer.h`). A render loop on another
core streaming a framebuffer and megabytes of texture data will continuously
evict core 0's preloaded lines — pure capacity contention, which no memory
barrier fixes. Circle's own `sysconfig.h` comment on `ARM_ALLOW_MULTI_CORE`
says as much: multiple cores may compete for bus time without use.

**The spike this implies**: core 1 needs to stream through a large DRAM
working set in a render-loop-shaped access pattern (not an isolated
register-resident counter, which proves nothing about cache contention),
while core 0 keeps running real REU/gpu64 traffic — verified byte-exact —
and milestone 3's mirror is confirmed to keep updating cleanly. "Is core 1
alive" is not the experiment; "does core 1's realistic memory traffic
disturb core 0's timing" is.

**Status: spiked on real hardware (2026-08-22), result negative, root cause
unresolved.** [`gpu64_multicore.h`/`.cpp`](../Source/Firmware/gpu64_multicore.h)
implements the above (cores 1-3 each stream writes through a 2MB buffer).
Starting it — even just having cores 1-3 alive and streaming, **before**
`reuUsingPolling()` is ever reached — produced garbage on the C64's own
native VIC-II output while trying to navigate RAD's menu. Disabling the
spike (`GPU64_MULTICORE_SPIKE_ENABLED` left undefined, see `rad_main.h`)
restored clean menu rendering, isolating the cause to the multicore startup
itself, not to `reuUsingPolling()` specifically. Two candidate explanations,
neither confirmed:

1. **Shared-L2 capacity contention**, as predicted above — though this
   would be expected to produce timing jitter/skipped cycles, not the kind
   of outright corruption "VIC-II garbage" suggests.
2. **A possible cache-coherency misconfiguration**, found while looking for
   why (1) alone seemed too mild an explanation: the custom armstub
   ([`rad-prefetch.S`](<../Source/Firmware/ARM STUB/rad-prefetch.S>)) sets
   `CPUECTLR_EL1` via `mov x0, #CPUECTLR_EL1_SMPEN; msr CPUECTLR_EL1, x0`,
   commented as "disable cache coherency." Per the standard Cortex-A53
   meaning of that bit, loading the `SMPEN` mask and writing it should
   *enable* SMP coherency participation, not disable it — the opposite of
   the comment, and of a commented-out `mov x0, #0` alternative right next
   to it. Not independently confirmed (needs an actual register readback
   or reference-manual check, not just reading the assembly) — but if this
   core was tuned assuming no cross-core coherency and multicore actually
   needs it, that would produce real memory-view corruption between cores,
   which fits the symptom better than pure contention does.

**Follow-up (2026-08-22), Opus 5 design review + a second hardware round:**
review of the first spike found a real confound — `CMultiCoreSupport::Initialize()`
does substantial work independent of what `Run()` does per core (permanently
enables `CSpinLock`'s real atomic path, rewrites interrupt routing, brings
each secondary through `EnableMMU()`/`EnableIRQs()` before `Run()` is ever
reached), so the original A/B never isolated bring-up from workload. The
review also mostly retired the SMPEN theory at the desk: the armstub's EL3
prologue runs identically on all four cores before the primary/secondary
split, so SMPEN is set the same way in both of the original test's builds —
it can't be what differed between them.

A follow-up build isolated the two: `GPU64_MULTICORE_ENABLED` on (full
bring-up — spinlocks live, interrupt routing rewritten, every secondary's
MMU/IRQs enabled) with `GPU64_MULTICORE_STRESS_ENABLED` off (every
secondary's `Run()` is a no-op). **Result: RAD menu stayed clean.**
Bring-up alone is not the cause — only the stress workload's actual memory
traffic corrupts things. This favors the shared-L2/bandwidth contention
theory over a fundamental coherency problem, and means multicore itself is
not off the table for milestone 6 — it means the eventual render loop needs
to be designed within a real memory-traffic budget, not assumed free, and
that budget still needs to be found.

Not investigated further for now — milestone 4 doesn't need multicore at
all (see milestone4_2d_api_design.md), so this isn't blocking. Starting point for
whoever picks milestone 6 back up: find the actual contention threshold
(a load ladder — one core, smaller/slower traffic patterns, working up
rather than starting from a deliberately worst-case synthetic stress) now
that bring-up itself is confirmed clean.

**That load ladder is now built** —
[`gpu64_ladder.h`/`.cpp`](../Source/Firmware/gpu64_ladder.cpp), one worker
core stepping through eight throttled bandwidth rungs from idle to
unthrottled, while core 0 counts the bus cycles it actually misses and the
C64 runs verified REU transfers and a vblank flip loop. It replaces the
"does the RAD menu look like garbage" signal with a per-rung number, which
is what makes rungs rankable at all. Design rationale, bench procedure and
how to read the table: progress_tracker.md milestone 6a. **That number has been found, and it is a
burst length rather than a bandwidth: see the top of this section.**

Also unverified: RAD disables IRQs (see progress_tracker.md's hw_testing
notes on why `CScreenDevice`/`CSerialDevice` logging stops working). Any
Circle facility core 1 wants to use that assumes a working interrupt system
(HDMI/timer/scheduler paths) needs checking against that constraint before
being relied on.

**Not yet decided**: whether this ends up needing exactly one worker core
or more (a future WiFi API is planned separately and will likely want its
own loop eventually) — start with core 0 + one worker, expand only once
that's measured working.

## Cache-preload requirements for the render loop

Beyond core 0's existing instruction-cache preload window (bounded at
`0x1a00` bytes, `reuUsingPolling()`), anything added to that loop for
milestone 6 (wider opcode dispatch, blob-descriptor parsing, ring-buffer
push) needs its footprint budgeted against that same bound — code that
grows past it is silently left unpreloaded, and the failure mode is timing
jitter, not a compile error. A DMA pull's destination (resource RAM, not
just the small static buffers `gpu64_mirrorSnapshot()` uses) needs the same
explicit cache-warming treatment `reuPrefetch()` gives `reuMemory`, or
per-byte cost stops being predictable once the destination isn't already
resident.

**Known unresolved precedent, worth re-checking here**: during milestone 3
bring-up, the polling loop itself froze (menu button unresponsive, not just
the display) once per-snapshot logging plus an in-loop cache clean pushed
enough extra work into the loop — root cause never conclusively identified
(see progress_tracker.md's "Known issues"). Milestone 6 proposes doing
substantially more work in that same place; the stage-indicator technique
built to localize that freeze (git history around `028fd01`) is worth
having ready again if something similar reappears.

## Resource lifecycle

Uploading `uploadTexture(id=5, ...)` allocates a chunk of resource RAM and
files it under ID 5.

**Decided:**
- **Re-upload to a live ID implicitly frees the old allocation and
  replaces it** — convenient for "reload this texture" without a
  free-then-upload dance.
- **Explicit `freeResource(id)` only, no automatic eviction.** Resource RAM
  is 512MB against a C64/REU-side source data ceiling of 16MB — running out
  is expected to be rare and can be a hard error. Automatic eviction would
  also add nondeterminism to GPU-side memory state, in the same spirit as
  core 0's cycle-predictability requirement (different layer, same
  principle).
- **All resources for the current session are freed on `resetREU()`** —
  same hook already used to fix the `gpu64ApiActive` sticky-flag bug
  (progress_tracker.md's milestone 3 section). Without this, RUN/STOP+
  RESTORE or a reset leaks the whole resource table and the next program
  starts against stale IDs.
- **One flat 16-bit ID namespace** across all resource types (textures,
  meshes, future kinds) — `ID` is already 16 bits wide in api_design.md's
  register map, no reason to split it per type. IDs are C64-assigned; two
  independent pieces of C64 code both picking the same ID will collide
  silently (acceptable for this project's scope — single program at a
  time, no OS — but worth stating explicitly rather than leaving it
  implicit).
- **`ERRCODE` needs an `OUT_OF_MEMORY` value** for the allocation-failure
  case above.

## Multi-blob commands (mesh upload, etc.)

api_design.md's single-blob descriptor convention (space/addr/len) extends
to commands needing more than one blob — e.g. mesh upload: vertex array +
index array. Two descriptors (12 bytes) fit in the 16-byte `ARG` block with
4 bytes left for command-specific fields.

An earlier draft proposed a third tier for 3+ blobs — the `ARG` descriptor
pointing at an indirect table of further descriptors in C64 RAM/REU, so
gpu64 pulls the table first, then each blob in turn. **Cut**: no in-scope
command needs more than two blobs, and designing a wire format for a case
that doesn't exist yet is speculative generality. If a genuine 3+-blob
command shows up, give it a bespoke `ARG` layout then, or split it into
multiple upload commands.

## The API, decided (2026-08-23)

Everything from here to the opcode table was settled in a design pass on
2026-08-23. It is design, not implementation — nothing below exists in
firmware yet, and none of it belongs in [api_design.md](api_design.md)
until it does.

### Shape of the thing

- **Textured, affine-mapped triangles**, flat-lit per face. Not wireframe,
  not Gouraud.
- **Both a retained scene and an immediate mode.** The retained scene plus
  the autonomous loop is the target end-state; `DRAW_MESH` exists because
  bring-up needs something that draws one mesh with no loop, no scene graph
  and no cross-core state to be wrong.
- **3D renders into a viewport**, not the whole screen; the C64 keeps
  drawing class-0 2D into the rest of the page (HUD, score, text).
- **16-bit z-buffer over that viewport.** See the budget below.

### Hidden-surface removal and the working-set budget

A z-buffer, sized to the viewport rather than to the screen, and the
rasteriser takes the region to fill as a parameter rather than assuming
the whole viewport.

Against round 5's finding, the set that matters is what the render loop
*writes*: `w * h` bytes of colour into the draw page plus `w * h * 2`
bytes of z. For the reference 256x160 viewport that is 120 KB, inside the
256 KB that measured free, with room left for texels — which are the read
traffic textures add on top, and which no HSR scheme avoids.

    GPU64_3D_BUDGET = 196608     /* w * h * 3, provisional */

`SET_VIEWPORT` rejects anything past that with `OUT_OF_RANGE`. The number
is provisional until ladder round 7 finds the actual edge; it leaves ~64 KB
of L2 for texels and for core 0's own preload schedule.

Painter's algorithm was considered and rejected: it saves the 80 KB of z
but breaks on interpenetrating geometry, and it saves the wrong resource —
with textures the pressure is read traffic, which sorting does not touch.
Full tile-based binning bounds both sets and is the eventual answer at
higher resolutions, but it is a binning pass plus a per-tile emit, and it
is the wrong thing to be debugging alongside a first affine rasteriser.
Keeping the region argument in the rasteriser's signature is what keeps
that upgrade a scheduling change instead of a rewrite.

### Lighting: a generated colormap, not a structured palette

The framebuffer stays **8bpp indexed all the way to scanout**. Shading that
emitted RGB was considered and rejected: the palette lookup is free today
because it happens in the display hardware at scanout, and RGB output would
make each page 128 KB, putting the written set near 380 KB before a single
texel — over budget, and it would break every class-0 op and the overlay,
which are defined in palette indices.

Carving the palette into ramps (64 colours x 4 shades, or 32 x 8) was also
rejected. It constrains artwork, and four shade levels band visibly on a
rotating face.

Instead, Doom's mechanism — the palette stays 256 freely-chosen colours,
and lighting goes through a generated table:

    COLORMAP[level][index] -> index          16 levels x 256 = 4 KB
    inner loop:   dst = colormap[light * 256 + texel]

`BUILD_COLORMAP` darkens every palette entry by each of 16 factors and
finds the nearest entry the palette actually has — 256 x 16 nearest-colour
searches, a few milliseconds, once. What this buys:

- Textures use all 256 colours with no imposed ramp layout.
- 16 light levels, smooth enough that rotation reads as shading.
- The table is 4 KB, so it is L1-resident: lighting costs one indexed byte
  load per pixel and **zero DRAM traffic**.
- It degrades gracefully — a palette with no dark blues maps dark blue to
  the closest thing it has. Quality tracks how well the palette covers the
  value range, which the artist controls.

A default palette of roughly 32 hues x 8 brightnesses covers that range
well, but that is advice to artists, not a wire-format constraint. Distance
fog is the same table with the level chosen from depth.

### Transforms: no matrices cross the bus

The GPU owns every matrix. The C64 never assembles, sends or sees one.

Each scene node — objects and cameras alike — holds a GPU-side position and
orientation, manipulated two ways:

| Style | Opcodes | Use |
|---|---|---|
| Absolute | `SET_POSITION`, `SET_ORIENTATION` | placing, teleporting, snapping a camera |
| Relative, **object-local** | `MOVE_LOCAL`, `ROTATE_LOCAL` | "forward 1.5, turn left" is one 8-byte command |

`MOVE_LOCAL` with only `dz` set is walking forward, whatever direction the
node faces — the GPU composes the delta against the stored orientation. A
camera is a node with a camera component, so the same four opcodes fly it.
Perspective is set once at instantiation (`SET_PERSPECTIVE`) and adjustable
later.

Wire formats, chosen for what a 6502 finds cheap:

| Quantity | Format | Why |
|---|---|---|
| Angles | 16-bit binary angle, 65536 = 360 deg | add a turn rate and let it wrap: no clamp, no modulo, no degrees/radians conversion. GPU takes sin/cos from a table. |
| Positions | signed 16.16, 4 bytes/axis | +/-32768 units at 1/65536 — a level, not just one model |
| Deltas | signed 8.8, 2 bytes/axis | a per-frame movement is small by definition, and this is the command that fires every frame |
| Scale | unsigned 8.8, uniform | non-uniform scale would need a normal fix-up in the rasteriser |

### Mesh format

The decisive framing: **the C64 never touches mesh bytes.** It points a blob
descriptor at data loaded from disk or already sitting in the REU; it does
not build or edit vertices at runtime. So "easy for the C64" is nearly free,
and the format is optimised for the rasteriser and for an offline exporter.

Two blobs, fitting the existing two-descriptor `ARG` layout:

    blob 0 — vertices, 6 bytes each, at most 256 per mesh
        x, y, z            signed 8.8, model space (model fits +/-128 units)

    blob 1 — faces, 12 bytes each
        i0, i1, i2         1 byte each, index into blob 0
        u0,v0, u1,v1, u2,v2  1 byte each, per-corner texcoords
        texid              1 byte, low byte of a texture resource ID
        flags              1 byte: bit0 double-sided, bit1 flat-colour
                           (texid is a palette index instead), bit2 unlit

Vertex and face counts are implied by each blob's `len` (`len/6`, `len/12`);
a `len` not divisible by the stride is `BAD_ARGS`.

Three deliberate calls:

- **Triangles only.** Quads save one index byte and one flags byte per pair
  of triangles — a quad still needs four corner UVs — and cost a second
  rasteriser path. The exporter triangulates.
- **UVs per face-corner, not per vertex.** Non-negotiable for texturing
  boxy geometry: a cube corner needs three different UVs for the three
  faces meeting there, and per-vertex UVs would force that vertex to be
  duplicated three times.
- **Normals computed GPU-side at upload**, from winding order. Nothing to
  author, smaller uploads, and flat per-face shading with texture detail is
  the chosen look. Smooth shading, if ever wanted, is an optional third
  blob behind a flag — not a format change.

**The 256-vertex cap is per mesh, not per scene.** An index byte is read
relative to that mesh's own vertex blob. The three limits are independent:

| Limit | Value | Set by |
|---|---|---|
| Vertices per mesh | 256 | the 1-byte face index |
| Meshes and textures resident | thousands | the flat 16-bit resource ID space, 512 MB of resource RAM |
| Object instances in the scene | 256 | the scene table, sized against the `SCENE_COMMIT` copy |

Splitting large geometry across meshes is wanted regardless of the index
width, because culling is per object: one giant mesh is all-or-nothing
against the frustum, while the same geometry as twelve meshes lets eleven
be rejected by a bounding-sphere test before a vertex is transformed. The
byte index makes that habit mandatory rather than merely advisable.

### Textures

Power-of-two dimensions, 8 to 256 on each side, not necessarily square, 8bpp
palette indices — the same palette as everything else. Power-of-two is what
makes the inner loop's wrap a mask rather than a modulo:

    texel = tex[((v & vmask) << wshift) | (u & umask)]

256 is the ceiling the byte UVs already imply. Anything else is `BAD_ARGS`.

### Frame lifecycle, and where the HUD fits

The render loop only ever writes pixels **inside the viewport**. The HUD
area of a page is the C64's alone, and nothing on core 1 touches it.

`LOOP_START` takes a mode:

- **Mode 0, handshake (default).** The loop renders a frame into the draw
  page and then stops, setting `STATUS` bit4 (frame-ready) and putting the
  page number in `RESULT`. The C64 draws its HUD into that page with
  ordinary class-0 ops, then calls `SCENE_COMMIT`, which does three things
  atomically: publishes the shadow scene, flips at the next vblank, and
  releases the loop to render the following frame into the other page. No
  races, no polling loop, no torn scene.
- **Mode 1, free-running.** The loop renders and flips on its own schedule
  and never waits for the C64 — the "play BASIC while the scene keeps
  rendering" case from the Philosophy section. `SCENE_COMMIT` still
  publishes scene updates atomically, but the HUD cannot be redrawn per
  frame, so whatever is outside the viewport is whatever was last drawn
  there on each page.

**Scene updates are double-buffered.** Every `SET_POSITION`/`MOVE_LOCAL`/
`SET_VISIBLE`/etc. writes into a shadow copy of the scene; `SCENE_COMMIT`
publishes it. A frame therefore never shows one object moved and another
not.

**Immediate mode requires the loop stopped.** `DRAW_MESH` returns `BUSY`
while the loop runs, keeping a single owner of the framebuffer and z-buffer
at any instant. The loop is off after `SCENE_RESET`, so bring-up is
immediate-mode only and the first rasteriser can be debugged without any
cross-core state at all.

### Register additions

Class 1 needs one readable byte class 0 does not have:

| Address | Name | Dir | Purpose |
|---|---|---|---|
| $DF21 | `RESULT` | R | Low byte of the last command's result — the page number from `SCENE_COMMIT`, the allocated ID from a `CREATE_*`. Meaning is per opcode; undefined for opcodes that define none. |

and one more `STATUS` bit:

| Bit | Meaning |
|---|---|
| 4 | frame-ready — the loop has finished a frame and is waiting for `SCENE_COMMIT` (handshake mode only) |

New `ERRCODE` values: `OUT_OF_MEMORY` (resource RAM exhausted, per Resource
lifecycle above), `QUEUE_FULL` (the core-0-to-core-1 ring buffer is full,
per Architecture above), `BAD_ID` (no such resource or node), `NO_CAMERA`
(a render was asked for with no active camera).

### Class 1 opcode table (draft)

`CMD_HI = 1`. `ARG` offsets are relative to `ARG0` ($DF11), exactly as in
class 0, and every opcode reads exactly the byte count in its row.

#### System and loop — $00-$0F

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $00 | `SCENE_RESET` | 0 | — | Destroys every node, stops the loop, leaves uploaded resources alone. |
| $01 | `SET_VIEWPORT` | 8 | x, y, w, h (16-bit each) | Places the 3D viewport in the page. `w*h*3 > GPU64_3D_BUDGET` is `OUT_OF_RANGE`; a viewport not wholly inside 320x200 is `BAD_ARGS`. |
| $02 | `SET_PERSPECTIVE` | 6 | `ARG0-1` fov (binary angle), `ARG2-3` near, `ARG4-5` far (8.8) | Projection for the active camera. |
| $03 | `SET_LIGHT` | 7 | `ARG0-5` direction x,y,z (8.8), `ARG6` ambient level 0-15 | The single directional light. Face shade = ambient + N.L, clamped to 0-15, indexing the colormap. |
| $04 | `BUILD_COLORMAP` | 0 | — | Regenerates the 16-level colormap from the current palette. Needed after `PAL_LOAD`/`PAL_SET`; costs milliseconds, never call it per frame. |
| $05 | `SET_BACKGROUND` | 1 | `ARG0` palette index | What the viewport is cleared to each frame. |
| $06 | `LOOP_START` | 1 | `ARG0` 0 = handshake, 1 = free-running | Starts the render loop. `NO_CAMERA` if none is active. |
| $07 | `LOOP_STOP` | 0 | — | Stops it after the current frame. |
| $08 | `SCENE_COMMIT` | 0 | — | Publishes the shadow scene; in handshake mode also flips at vblank and releases the next frame. `RESULT` = the page the just-finished frame is in. |
| $09 | `ARENA_STATUS` | 0 | — | `RESULT` = free resource RAM in 128 KB units. Added in phase 1; see below. |

#### Resources — $10-$1F

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $10 | `UPLOAD_MESH` | 12 | `ARG0-5` vertex blob, `ARG6-11` face blob | Uploads into resource RAM under the staged `ID`. Re-upload to a live ID replaces it (see Resource lifecycle). |
| $11 | `UPLOAD_TEXTURE` | 8 | `ARG0-5` blob, `ARG6` w shift, `ARG7` h shift | Dimensions are `1 << shift`, 3..8. `len` must equal `w*h`. |
| $12 | `FREE_RESOURCE` | 0 | — | Frees the staged `ID`. |

#### Scene nodes — $20-$2F

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $20 | `CREATE_OBJECT` | 2 | `ARG0-1` mesh resource ID | Creates a node instancing that mesh, under the staged node `ID`. |
| $21 | `CREATE_CAMERA` | 0 | — | Creates a camera node under the staged `ID`. |
| $22 | `DESTROY_NODE` | 0 | — | Destroys the staged `ID`. |
| $23 | `SET_ACTIVE_CAMERA` | 0 | — | The staged `ID` becomes the camera the loop renders from. |
| $24 | `SET_VISIBLE` | 1 | `ARG0` 0 or 1 | Skips the node without destroying it. |

#### Transforms — $30-$3F

All act on the staged node `ID`, and all write into the shadow scene.

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $30 | `SET_POSITION` | 12 | x, y, z (16.16) | Absolute, world space. |
| $31 | `SET_ORIENTATION` | 6 | yaw, pitch, roll (binary angle) | Absolute. |
| $32 | `MOVE_LOCAL` | 6 | dx, dy, dz (8.8) | Translate along the node's own axes. |
| $33 | `MOVE_WORLD` | 6 | dx, dy, dz (8.8) | Translate along world axes. |
| $34 | `ROTATE_LOCAL` | 6 | dyaw, dpitch, droll (binary angle) | Compose onto the current orientation. |
| $35 | `SET_SCALE` | 2 | s (unsigned 8.8) | Uniform. |
| $36 | `GET_TRANSFORM` | 6 | `ARG0-5` destination descriptor | Writes 18 bytes — position (12) + orientation (6) — back to C64 RAM or REU, for collision and gameplay logic. |

#### Immediate mode — $40-$4F

Legal only with the loop stopped; otherwise `BUSY`.

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $40 | `CLEAR_VIEWPORT` | 0 | — | Fills the viewport with the background index and clears the z-buffer. |
| $41 | `DRAW_MESH` | 14 | `ARG0-5` position x,y,z (8.8), `ARG6-11` orientation, `ARG12-13` scale; mesh resource in `ID` | Transforms, lights, clips and rasterises one mesh into the draw page's viewport, z-tested. Returns when done. |
| $42 | `DRAW_NODE` | 0 | — | Same, for a scene node already positioned via $30-$35 — no re-staging of a transform. |

## Phase 1, as built (2026-08-24)

Everything above is design. This section is the record of what the first
implementation actually settled, including the places it departs from the
sketch above. Status and history stay in
[progress_tracker.md](progress_tracker.md); what is here is API and
architecture, because a reader of this file needs it to write against the
thing that exists.

### The renderer runs on core 0, and immediate mode is why

The architecture section puts the render loop on core 1. Phase 1 does not:
`CLEAR_VIEWPORT` and `DRAW_MESH` execute synchronously on core 0, inside the
dispatch window where the C64 is already DMA-halted, exactly like every
class 0 draw op.

This is not a retreat from the design — it falls out of what immediate mode
already is. `DRAW_MESH` is specified to return when the mesh is drawn and to
be illegal while the loop runs, so there is exactly one owner of the
framebuffer and the z-buffer at any instant. Making that owner core 0 means
the whole pipeline — transform, cull, light, clip, rasterise — can be brought
up on hardware with the cross-core question still open, which is the same
reason `DRAW_MESH` exists at all.

The ring is still fed: every accepted class 1 command is pushed onto it and
counted by core 1, so the cross-core path stays under real command traffic
while the pipeline is built. Core 1 executes nothing.

`LOOP_START` and `SCENE_COMMIT` answer `UNSUPPORTED` — the opcode is real and
this build cannot do it, which is a different thing for a program to branch on
than an opcode that does not exist. The scene-node and transform opcodes
($20-$36) answer `BAD_OPCODE` until phase 2 implements them.

### The arena is owned by core 0

The Resource lifecycle section had core 1 allocating. It does not: an upload
is a DMA pull off the C64 bus, which only core 0 can do, so core 1 allocating
would mean core 0 pulling into a staging buffer and core 1 copying it out
again, with a hand-off protocol to stop core 0 reusing the staging buffer too
early. Core 0 allocating removes the copy and the protocol together, and there
is still no lock, still for the original reason: nothing core 0 could block on.

**The allocator is bump-only, so `FREE_RESOURCE` and re-upload reclaim the
table slot but not the bytes.** The whole arena comes back at session reset.
This is a phase-1 limit, stated rather than hidden: a program that re-uploads
in a loop will exhaust the arena and get `OUT_OF_MEMORY`, which is at least a
truthful error. The arena is 32 MB.

Because "you get `OUT_OF_MEMORY` eventually" is truthful and undebuggable
against 32 MB, **`ARENA_STATUS` ($09) makes the remaining space readable**:
`RESULT` is the free arena in 128 KB units, which is exactly 0..256 for a
32 MB arena and so fits RESULT's single byte. The HDMI log's `a####` field
carries the same number in KB for the case where no program is driving.

`Source/TestPRG/gpu64_3d_arena_test.a` drives the arena to exhaustion on
purpose — 8192 uploads of the same 4 KB texture to the same live ID, which is
exactly 32 MB — and asserts that the error comes back as `OUT_OF_MEMORY`. An
error path that has never fired is not a tested error path, and this one is
now cheap to fire.

### A class 1 dispatch must re-warm the polling loop

`DRAW_MESH` runs inside the dispatch window and walks a framebuffer, a
z-buffer and up to 32 MB of arena. That evicts the caches the polling loop
depends on, and the project already has a rule about it — rule 4 of the
polling-loop timing rules: *"preloaded at start-up" is not durable*. A single
class 0 `CLEAR` walking 64000 bytes was enough to evict `warmCache()`'s work;
a renderer does far more.

Two specific casualties:

- **The polling loop's instruction window.** This one partly self-heals — the
  loop rolls a `CACHE_PRELOADIKEEP` 64 bytes forward every pass across the
  `GPU64_POLL_IPL_WINDOW` — but that is ~106 passes to come fully back, and
  the loop runs cold for all of them.
- **`gpu64Regs` and `gpu64Vsync`.** `warmCache()` preloads these `L1KEEP`
  precisely because the loop touches them on every ARG write and every STATUS
  read, and nothing re-warms them after a dispatch.

`PAGE_FLIP` already faced exactly this and solved it: `gpu64_vsyncWarmCommit()`
re-warms from the dispatch that queues the flip. That is rule 5 — warm from
upstream, where the bus is already held. Class 1 does the same, through
`gpu64_apiWarmPollingLoop()` (rad_reu.cpp), called at the end of every class 1
dispatch before DMA release. The cost lands inside the window the C64 is
already stopped for, so it is free of the rule-5 trap.

It is called unconditionally rather than only after the heavy opcodes: a
dispatch is the biggest instruction-cache consumer in the system whatever it
does, and the warm is one window preload and two data preloads against a
dispatch that has already paid for a DMA round trip.

**Class 0's heavy opcodes have the same exposure and do not do this.** `CLEAR`,
`BLIT` and a 64 KB blob transfer all evict the same lines, and only the flip
path re-warms. That is a pre-existing gap, not one phase 1 introduced, and it
is left alone here rather than changed underneath a hardware-verified path —
but it is worth a deliberate decision rather than an omission.

### The store-burst budget is not yet load-bearing — and will be

`gpu64_3d_span.h` carries milestone 6a's budget and the yield that enforces
it, and the rasteriser already chunks its inner loop at
`GPU64_3D_SPAN_BYTES / 3` pixels (three bytes leave the core per pixel: one
colour, two z).

**None of that is under test today.** With the renderer on core 0 the C64 is
halted for the whole render, so nothing is contending and the budget costs a
`DSB` that protects against nothing. The moment phase 2 or phase 4 moves the
rasteriser to core 1, the budget becomes load-bearing for the first time, and
the constraint is hard: the limit is a **7-line store burst**, with burst
length the only axis that matters — rate, footprint and L2 residency all
turned out not to be. A span-filling inner loop is a long store burst by
nature, so what the rasteriser needs is chunking, which it has, not merely a
yield at triangle boundaries, which would be useless.

Two more rules that bind core 1 the day it renders:

- **Core 1 must never spin on an MMIO register while waiting for work.** Any
  MMIO poll from a second core breaks core 0's bus timing — that was ladder
  round 1, and it corrupted command arguments while writing zero bytes of
  DRAM traffic. Use `CNTVCT_EL0`, which is core-local and costs no bus cycle.
  The current worker parks on `WFE` against the generic-timer event stream,
  which satisfies this.
- **Core 0 must not read a line core 1 writes.** The ring's cached-tail design
  is what satisfies this: on a ring that is not full, core 0 never touches
  core 1's line at all. Separating the cache lines was *not* what fixed
  milestone 6a round 10 — removing core 0's read was.

### Conventions the wire format did not pin down

- **Axes are left-handed**: +x right, +y up, +z away from the camera.
- **Winding is clockwise as seen from outside the model.** A face is
  front-facing when its normal points back towards the camera; the normal
  comes from `cross(v1-v0, v2-v0)`. Get this backwards and every face of a
  closed mesh is culled, which looks exactly like an upload that never
  arrived — which is why `DRAW_MESH` returns a triangle count.
- **A positive pitch tips a node's own +z towards -y**, so a camera with a
  positive pitch looks *down*.
- **Face blob byte 11 is padding.** The eleven meaningful bytes are
  `i0,i1,i2, u0,v0,u1,v1,u2,v2, texid, flags`; the stride the design fixed at
  12 is a power of two so faces do not straddle cache lines.
- **A texcoord byte is a texel coordinate directly**, wrapped by the
  rasteriser's mask — so 0..255 addresses a 256-wide texture exactly and tiles
  a narrower one.
- **Colormap level 15 is full brightness** and resolves to the identity; level
  0 is 1/15 of the way up, not black, so a face turned fully away still reads
  as its own colour rather than as a hole.
- **The z-buffer holds `near/z`**, so a bigger value is nearer and an
  untouched pixel (0) loses to every real fragment.

### Limits the formats impose, discovered by using them

- **`near` and `far` are 8.8, so the far plane cannot exceed 127.99 units.**
  A scene lives at z=70, not z=700. Nothing in the design says otherwise; it
  is simply not obvious until a demo is written. **This is a scale convention,
  not a distance ceiling** — a scene is authored in units where the far plane
  fits, exactly as a model is authored to fit the 8.8 vertex format's ±128.
  Nothing about the pipeline stops a "kilometre" being one unit. What it does
  mean is that the unit has to be chosen once, up front, for the whole scene.
- **The z-buffer holds `near/z`, so depth precision is set entirely by
  `near`.** A needlessly small near plane throws it away: half the buffer's
  range is spent between `near` and `2 * near`, wherever that happens to be.
  The instinct is to set near tiny so nothing ever clips; the cost is z
  fighting everywhere else in the scene. Set it as far out as the closest
  thing the camera will ever get to.
- Rotation matrices are held internally in **1.15**, not 8.8. A matrix entry
  quantised to 1/256 puts a rotating box's edge a whole pixel from where it
  belongs at 320 px wide, and the error is systematic, so it reads as wobble.
- `RESULT` after `DRAW_MESH` is the **triangle count**, saturated to a byte.
  `UPLOAD_MESH` leaves the face count there.

### The renderer is portable, and is tested on a PC

`gpu64_3d_math`, `gpu64_3d_raster`, `gpu64_3d_colormap` and `gpu64_3d_render`
depend on nothing but `<circle/types.h>`. `tools/hostsim` compiles them
unchanged with a native g++ against a stubbed header and writes PPM images:
a turning box, a z-buffer interpenetration case, a near-plane straddle, the
flat-colour path, and the exact scene `Source/TestPRG/gpu64_3d_cube.a` asks
for — which gives a bench run a reference picture to hold the HDMI output
against.

This is not a nicety. The first thing it found was four transposed signs in
the Euler matrix, which do not look like a sign error: they make the matrix
non-orthogonal, so the model *shears* as it turns and reads as a perspective
artefact. Finding that on hardware would have cost a bench session.

## Open questions


1. Core-split feasibility (see Architecture above) — ladder round 7's edge
   is the number `GPU64_3D_BUDGET` is provisionally guessing at. Nothing in
   the API above is invalidated by a lower edge; the viewport just gets
   smaller, and the rasteriser's region argument is what makes tiling
   available if it gets much lower.
2. Whether the scene graph ever needs **hierarchy** (a turret parented to a
   tank). Deliberately not designed — `SET_PARENT` is a one-opcode addition
   later, and composing parent transforms every frame is work core 1 does
   not have to do until something asks for it.
3. **Clipping strategy** for triangles crossing the near plane — full
   Sutherland-Hodgman against the frustum, or near-plane only plus 2D
   scissoring against the viewport rect. The second is cheaper and is
   probably right, but it wants measuring against real geometry.
4. Whether a **bounding sphere** is computed at `UPLOAD_MESH` (cheap, and
   per-object frustum culling wants it) or supplied by the exporter.
