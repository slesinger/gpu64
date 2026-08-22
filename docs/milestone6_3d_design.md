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
  cycle-predictability requirement (api_design.md) applies here too.

### The real risk: shared L2, not core-count

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

Not investigated further for now — milestone 4 doesn't need multicore at
all (see api_design.md), so this isn't blocking. Starting point for
whoever picks milestone 6 back up: resolve the SMPEN question first (it
changes which of the two theories above is even in play), then retry the
spike incrementally (one stress core at a time, smaller buffers) rather
than all three at full intensity as the first cut did.

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

## Open questions

1. Core-split feasibility (see Architecture above) — prototype with the
   corrected DRAM-stress spike before designing further on top of it.
2. Mesh upload's exact `ARG` layout (stride, attribute layout, interleaved
   vs. separate arrays) — bespoke per command, design when the opcode table
   gets built.
3. Full class-1 (3D) opcode table.
